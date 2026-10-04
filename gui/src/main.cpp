#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include "vulkan.h"
#include "font_roboto.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <filesystem>
#include <chrono>
#include <cctype>
#include <algorithm>

namespace fs = std::filesystem;

// ===========================================================================================================
// String & Filesystem Helpers
// ===========================================================================================================

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, nullptr, nullptr);
    return str;
}

static std::string SanitizeFilename(const std::string& name, size_t max_len = 100) {
    std::string clean;
    for (char c : name) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"'  || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
            continue;
        }
        clean += c;
    }
    // collapse consecutive spaces
    std::string result;
    bool last_space = false;
    for (char c : clean) {
        if (c == ' ') {
            if (!last_space) { result += c; last_space = true; }
        } else {
            result += c;
            last_space = false;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.size() > max_len) result = result.substr(0, max_len);
    return result.empty() ? "unnamed" : result;
}

static fs::path GetExecutableDirectory() {
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        return fs::path(buf).parent_path();
    }
    return fs::current_path();
}

// ===========================================================================================================
// Lightweight JSON Parser
// ===========================================================================================================

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json& operator[](const std::string& key) const {
        static Json null_val;
        if (type != Object) return null_val;
        for (const auto& kv : o) {
            if (kv.first == key) return kv.second;
        }
        return null_val;
    }

    const Json& operator[](size_t idx) const {
        static Json null_val;
        if (type != Array || idx >= a.size()) return null_val;
        return a[idx];
    }

    bool is_null() const { return type == Null; }
};

struct JsonParser {
    const std::string& src;
    size_t i = 0;

    void skip() {
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) {
            i++;
        }
    }

    std::string parse_string() {
        std::string res;
        if (i >= src.size() || src[i] != '"') return res;
        i++;
        while (i < src.size()) {
            char c = src[i++];
            if (c == '"') break;
            if (c == '\\' && i < src.size()) {
                char esc = src[i++];
                if (esc == '"') res += '"';
                else if (esc == '\\') res += '\\';
                else if (esc == '/') res += '/';
                else if (esc == 'b') res += '\b';
                else if (esc == 'f') res += '\f';
                else if (esc == 'n') res += '\n';
                else if (esc == 'r') res += '\r';
                else if (esc == 't') res += '\t';
                else if (esc == 'u' && i + 4 <= src.size()) i += 4;
            } else {
                res += c;
            }
        }
        return res;
    }

    Json parse_value() {
        skip();
        if (i >= src.size()) return {};
        char c = src[i];
        if (c == '"') {
            Json j; j.type = Json::String; j.s = parse_string(); return j;
        }
        if (c == '{') {
            i++;
            Json j; j.type = Json::Object;
            while (true) {
                skip();
                if (i < src.size() && src[i] == '}') { i++; break; }
                if (i >= src.size() || src[i] != '"') break;
                std::string key = parse_string();
                skip();
                if (i < src.size() && src[i] == ':') i++;
                Json val = parse_value();
                j.o.push_back({ key, val });
                skip();
                if (i < src.size() && src[i] == ',') i++;
                else if (i < src.size() && src[i] == '}') { i++; break; }
                else break;
            }
            return j;
        }
        if (c == '[') {
            i++;
            Json j; j.type = Json::Array;
            while (true) {
                skip();
                if (i < src.size() && src[i] == ']') { i++; break; }
                Json val = parse_value();
                j.a.push_back(val);
                skip();
                if (i < src.size() && src[i] == ',') i++;
                else if (i < src.size() && src[i] == ']') { i++; break; }
                else break;
            }
            return j;
        }
        if (c == 't' || c == 'f') {
            Json j; j.type = Json::Bool;
            if (i + 4 <= src.size() && src.substr(i, 4) == "true") { j.b = true; i += 4; }
            else if (i + 5 <= src.size() && src.substr(i, 5) == "false") { j.b = false; i += 5; }
            return j;
        }
        if (c == 'n') {
            if (i + 4 <= src.size() && src.substr(i, 4) == "null") i += 4;
            return {};
        }
        size_t start = i;
        if (c == '-') i++;
        while (i < src.size() && (std::isdigit((unsigned char)src[i]) || src[i] == '.' ||
                                  src[i] == 'e' || src[i] == 'E' || src[i] == '+' || src[i] == '-')) {
            i++;
        }
        Json j; j.type = Json::Number;
        j.n = std::strtod(src.c_str() + start, nullptr);
        return j;
    }
};

// ===========================================================================================================
// Native WinHTTP Client
// ===========================================================================================================

class WinHttpClient {
public:
    WinHttpClient() {
        m_session = WinHttpOpen(
            L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
    }

    ~WinHttpClient() {
        if (m_session) WinHttpCloseHandle(m_session);
    }

    std::string GetString(const std::string& url_utf8, int max_retries = 3) {
        std::wstring wurl = Utf8ToWide(url_utf8);

        URL_COMPONENTS urlComp = { sizeof(URL_COMPONENTS) };
        wchar_t hostName[256] = { 0 };
        wchar_t urlPath[2048] = { 0 };
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = 2048;

        if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.length(), 0, &urlComp)) {
            return "";
        }

        for (int attempt = 0; attempt < max_retries; ++attempt) {
            HINTERNET hConnect = WinHttpConnect(m_session, hostName, urlComp.nPort, 0);
            if (!hConnect) continue;

            DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect, L"GET", urlPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
            );

            if (!hRequest) {
                WinHttpCloseHandle(hConnect);
                continue;
            }

            LPCWSTR headers = L"Origin: https://app.astrobin.com\r\nReferer: https://app.astrobin.com/\r\nAccept: application/json, text/plain, */*\r\n";
            BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)wcslen(headers), nullptr, 0, 0, 0);
            if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, statusSize = sizeof(status);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr);

                if (status == 200) {
                    std::string result;
                    char buf[4096];
                    DWORD bytesRead = 0;
                    while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                        result.append(buf, bytesRead);
                    }
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    return result;
                }
            }

            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
        }
        return "";
    }

    bool DownloadFile(const std::string& url_utf8, const fs::path& dest_path, const std::atomic<bool>& stop_flag) {
        std::wstring wurl = Utf8ToWide(url_utf8);

        URL_COMPONENTS urlComp = { sizeof(URL_COMPONENTS) };
        wchar_t hostName[512] = { 0 };
        wchar_t urlPath[4096] = { 0 };
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = 512;
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = 4096;

        if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.length(), 0, &urlComp)) {
            return false;
        }

        HINTERNET hConnect = WinHttpConnect(m_session, hostName, urlComp.nPort, 0);
        if (!hConnect) return false;

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", urlPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
        );

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            return false;
        }

        LPCWSTR headers = L"Referer: https://app.astrobin.com/\r\n";
        if (!WinHttpSendRequest(hRequest, headers, (DWORD)wcslen(headers), nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            return false;
        }

        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr);
        if (status != 200) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            return false;
        }

        // Create target directory if needed
        std::error_code ec;
        fs::create_directories(dest_path.parent_path(), ec);

        fs::path temp_path = dest_path;
        temp_path += ".part";

        std::ofstream out(temp_path, std::ios::binary);
        if (!out.is_open()) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            return false;
        }

        char buffer[32768];
        DWORD bytesRead = 0;
        bool success = true;

        while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            if (stop_flag.load()) {
                success = false;
                break;
            }
            out.write(buffer, bytesRead);
        }

        out.close();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);

        if (success && !stop_flag.load()) {
            if (fs::exists(dest_path, ec)) fs::remove(dest_path, ec);
            fs::rename(temp_path, dest_path, ec);
            return !ec;
        } else {
            fs::remove(temp_path, ec);
            return false;
        }
    }

private:
    HINTERNET m_session = nullptr;
};

// ===========================================================================================================
// Native AstroBin Crawler Engine
// ===========================================================================================================

struct TargetInfo {
    enum Mode { Unknown, UserGallery, SingleImage };
    Mode mode = Unknown;
    std::string username;
    std::string hash;
};

static TargetInfo ParseTarget(std::string target) {
    TargetInfo info;
    while (!target.empty() && (target.front() == ' ' || target.front() == '\t')) target.erase(0, 1);
    while (!target.empty() && (target.back() == ' ' || target.back() == '\t')) target.pop_back();
    if (target.empty()) return info;

    // Check query params ?i=...
    size_t q_pos = target.find("?i=");
    if (q_pos != std::string::npos) {
        info.mode = TargetInfo::SingleImage;
        info.hash = target.substr(q_pos + 3);
        size_t amp = info.hash.find('&');
        if (amp != std::string::npos) info.hash = info.hash.substr(0, amp);
        return info;
    }

    // Direct /i/<hash>
    size_t i_pos = target.find("/i/");
    if (i_pos != std::string::npos) {
        info.mode = TargetInfo::SingleImage;
        info.hash = target.substr(i_pos + 3);
        while (!info.hash.empty() && (info.hash.back() == '/' || info.hash.back() == '#')) info.hash.pop_back();
        return info;
    }

    // Profile URL: app.astrobin.com/u/<username>
    size_t u_pos = target.find("/u/");
    if (u_pos != std::string::npos) {
        info.mode = TargetInfo::UserGallery;
        info.username = target.substr(u_pos + 3);
        size_t slash = info.username.find('/');
        if (slash != std::string::npos) info.username = info.username.substr(0, slash);
        size_t hash_ch = info.username.find('#');
        if (hash_ch != std::string::npos) info.username = info.username.substr(0, hash_ch);
        size_t qm = info.username.find('?');
        if (qm != std::string::npos) info.username = info.username.substr(0, qm);
        return info;
    }

    // Classic users/<username>
    size_t users_pos = target.find("/users/");
    if (users_pos != std::string::npos) {
        info.mode = TargetInfo::UserGallery;
        info.username = target.substr(users_pos + 7);
        size_t slash = info.username.find('/');
        if (slash != std::string::npos) info.username = info.username.substr(0, slash);
        return info;
    }

    if (target.front() == '@') {
        info.mode = TargetInfo::UserGallery;
        info.username = target.substr(1);
        return info;
    }

    // 6-character hash
    if (target.size() == 6) {
        bool all_alnum = true;
        for (char c : target) if (!std::isalnum((unsigned char)c)) all_alnum = false;
        if (all_alnum) {
            info.mode = TargetInfo::SingleImage;
            info.hash = target;
            return info;
        }
    }

    info.mode = TargetInfo::UserGallery;
    info.username = target;
    return info;
}

static std::string GetBestThumbnailUrl(WinHttpClient& client, const std::string& hash, const std::string& revision = "0") {
    const char* aliases[] = { "real", "qhd", "hd", "regular" };
    for (const char* alias : aliases) {
        std::string thumb_url = "https://www.astrobin.com/" + hash + "/" + revision + "/thumb/" + alias + "/";
        std::string json_str = client.GetString(thumb_url);
        if (!json_str.empty()) {
            Json j = JsonParser{ json_str }.parse_value();
            std::string url = j["url"].s;
            if (!url.empty() && url.find("placeholder") == std::string::npos && url.find("loading.gif") == std::string::npos) {
                return url;
            }
        }
    }
    return "";
}

// ===========================================================================================================
// Application State & Crawler Manager
// ===========================================================================================================

enum class CrawlStatus {
    Idle,
    Running,
    Completed,
    Failed,
    Stopped
};

struct ImageTask {
    std::string hash;
    std::string title;
    std::string user;
};

struct CrawlSession {
    std::atomic<CrawlStatus> status{CrawlStatus::Idle};
    std::atomic<bool> stop_requested{false};

    std::mutex log_mutex;
    std::vector<std::string> logs;
    bool auto_scroll = true;

    std::atomic<int> total_images{0};
    std::atomic<int> completed_images{0};

    std::thread crawler_thread;

    void AppendLog(const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.push_back(line);
        if (logs.size() > 2500) {
            logs.erase(logs.begin(), logs.begin() + 500);
        }
    }

    void ClearLogs() {
        std::lock_guard<std::mutex> lock(log_mutex);
        logs.clear();
    }
};

static void RunNativeCrawler(
    CrawlSession& session,
    std::string target,
    std::string output_dir_str,
    int worker_count,
    int limit,
    bool save_metadata
) {
    if (session.status == CrawlStatus::Running) return;

    session.status = CrawlStatus::Running;
    session.stop_requested = false;
    session.total_images = 0;
    session.completed_images = 0;

    session.AppendLog("Parsing target: " + target);

    if (session.crawler_thread.joinable()) {
        session.crawler_thread.join();
    }

    session.crawler_thread = std::thread([&session, target, output_dir_str, worker_count, limit, save_metadata]() {
        WinHttpClient client;

        fs::path base_out = fs::path(output_dir_str);
        if (base_out.is_relative()) {
            base_out = GetExecutableDirectory() / base_out;
        }

        TargetInfo info = ParseTarget(target);
        if (info.mode == TargetInfo::Unknown) {
            session.AppendLog("Error: Unrecognized target format: " + target);
            session.status = CrawlStatus::Failed;
            return;
        }

        std::vector<ImageTask> tasks;

        if (info.mode == TargetInfo::SingleImage) {
            session.AppendLog("Fetching details for image: " + info.hash);
            std::string detail_url = "https://app.astrobin.com/api/v2/images/image/?hash=" + info.hash;
            std::string resp = client.GetString(detail_url);
            std::string title = info.hash;
            std::string username = "standalone";
            if (!resp.empty()) {
                Json j = JsonParser{ resp }.parse_value();
                if (j["results"].type == Json::Array && j["results"].a.size() > 0) {
                    title = j["results"][0]["title"].s;
                    username = j["results"][0]["user"].s;
                }
            }
            tasks.push_back({ info.hash, title, username });
        } else if (info.mode == TargetInfo::UserGallery) {
            session.AppendLog("Resolving user @" + info.username + "...");
            std::string user_url = "https://app.astrobin.com/api/v2/common/users/?username=" + info.username;
            std::string user_resp = client.GetString(user_url);
            if (user_resp.empty()) {
                session.AppendLog("Error: User @" + info.username + " not found or network error.");
                session.status = CrawlStatus::Failed;
                return;
            }

            Json user_j = JsonParser{ user_resp }.parse_value();
            if (user_j.type != Json::Array || user_j.a.empty()) {
                session.AppendLog("Error: Could not find user: @" + info.username);
                session.status = CrawlStatus::Failed;
                return;
            }

            int user_id = (int)user_j[0]["id"].n;
            session.AppendLog("Found user @" + info.username + " (ID: " + std::to_string(user_id) + "). Fetching gallery...");

            int page = 1;
            while (!session.stop_requested.load()) {
                std::string page_url = "https://app.astrobin.com/api/v2/images/image/?user=" +
                                       std::to_string(user_id) + "&gallery-serializer=1&page=" + std::to_string(page);
                std::string page_resp = client.GetString(page_url);
                if (page_resp.empty()) break;

                Json gal_j = JsonParser{ page_resp }.parse_value();
                const Json& results = gal_j["results"];
                if (results.type != Json::Array || results.a.empty()) break;

                for (size_t i = 0; i < results.a.size(); ++i) {
                    std::string h = results[i]["hash"].s;
                    std::string t = results[i]["title"].s;
                    if (!h.empty()) {
                        tasks.push_back({ h, t, info.username });
                        if (limit > 0 && (int)tasks.size() >= limit) break;
                    }
                }

                if (limit > 0 && (int)tasks.size() >= limit) break;
                if (gal_j["next"].is_null() || gal_j["next"].s.empty()) break;

                page++;
            }
        }

        if (session.stop_requested.load()) {
            session.AppendLog("Crawl stopped by user.");
            session.status = CrawlStatus::Stopped;
            return;
        }

        if (tasks.empty()) {
            session.AppendLog("No images found to download.");
            session.status = CrawlStatus::Completed;
            return;
        }

        session.total_images = (int)tasks.size();
        session.AppendLog("Found " + std::to_string(tasks.size()) + " images to download. Starting workers...");

        // Concurrent Task Queue
        std::mutex queue_mutex;
        std::queue<ImageTask> task_queue;
        for (const auto& t : tasks) task_queue.push(t);

        int num_threads = std::max(1, std::min(worker_count, 16));
        std::vector<std::thread> workers;

        for (int t = 0; t < num_threads; ++t) {
            workers.emplace_back([&]() {
                WinHttpClient worker_client;
                while (!session.stop_requested.load()) {
                    ImageTask cur;
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        if (task_queue.empty()) break;
                        cur = task_queue.front();
                        task_queue.pop();
                    }

                    // Download logic
                    std::string cdn_url = GetBestThumbnailUrl(worker_client, cur.hash, "0");
                    if (cdn_url.empty()) {
                        session.AppendLog("Warning: Could not get download URL for " + cur.hash);
                        session.completed_images++;
                        continue;
                    }

                    std::string safe_title = SanitizeFilename(cur.title);
                    std::string filename = cur.hash + "_" + safe_title + ".jpg";

                    fs::path save_dir = base_out;
                    if (!cur.user.empty()) save_dir /= cur.user;
                    fs::path save_file = save_dir / filename;

                    std::error_code ec;
                    if (fs::exists(save_file, ec)) {
                        session.AppendLog("Skipping (already exists): " + filename);
                        session.completed_images++;
                        continue;
                    }

                    session.AppendLog("Downloading: " + filename + " ...");
                    bool ok = worker_client.DownloadFile(cdn_url, save_file, session.stop_requested);

                    if (ok && !session.stop_requested.load()) {
                        session.completed_images++;
                        session.AppendLog("Downloaded: " + filename + " (" +
                                          std::to_string(session.completed_images.load()) + "/" +
                                          std::to_string(session.total_images.load()) + ")");

                        // Metadata companion
                        if (save_metadata) {
                            std::string meta_url = "https://app.astrobin.com/api/v2/images/image/?hash=" + cur.hash;
                            std::string meta_json = worker_client.GetString(meta_url);
                            if (!meta_json.empty()) {
                                fs::path meta_file = save_dir / (cur.hash + "_" + safe_title + ".json");
                                std::ofstream mf(meta_file);
                                if (mf.is_open()) mf << meta_json;
                            }
                        }
                    } else if (session.stop_requested.load()) {
                        break;
                    } else {
                        session.AppendLog("Failed downloading: " + filename);
                        session.completed_images++;
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        if (session.stop_requested.load()) {
            session.status = CrawlStatus::Stopped;
            session.AppendLog("Crawl stopped by user.");
        } else {
            session.status = CrawlStatus::Completed;
            session.AppendLog("Finished! All images downloaded to: " + base_out.string());
        }
    });
}

static void StopNativeCrawler(CrawlSession& session) {
    if (session.status == CrawlStatus::Running) {
        session.stop_requested = true;
    }
}

// ===========================================================================================================
// Beautiful Minimalist Theme
// ===========================================================================================================

static void ApplyBeautifiedMinimalistTheme() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;

    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(22.0f, 20.0f);
    style.FramePadding      = ImVec2(14.0f, 9.0f);
    style.ItemSpacing       = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize     = 12.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.53f, 0.60f, 1.00f);

    colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.04f, 0.05f, 0.07f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);

    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.22f, 0.28f, 0.60f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_FrameBg]               = ImVec4(0.10f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.15f, 0.19f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.18f, 0.23f, 0.31f, 1.00f);

    colors[ImGuiCol_Button]                = ImVec4(0.13f, 0.17f, 0.23f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.18f, 0.24f, 0.33f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.10f, 0.14f, 0.20f, 1.00f);

    colors[ImGuiCol_Header]                = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.17f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);

    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.30f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);

    colors[ImGuiCol_Separator]             = ImVec4(0.16f, 0.20f, 0.26f, 0.70f);
}

// ===========================================================================================================
// Application Entry Point
// ===========================================================================================================

int main() {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(800, 560, "AstroBin Crawler", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface;
    check_vk_result(glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface));

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyBeautifiedMinimalistTheme();

    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    ImFont* robotoFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<uint8_t*>(g_RobotoRegular), sizeof(g_RobotoRegular), 18.0f, &font_cfg
    );
    if (robotoFont) io.FontDefault = robotoFont;

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance        = g_Instance;
    init_info.PhysicalDevice  = g_PhysicalDevice;
    init_info.Device          = g_Device;
    init_info.QueueFamily     = g_QueueFamily;
    init_info.Queue           = g_Queue;
    init_info.DescriptorPool  = g_DescriptorPool;
    init_info.MinImageCount   = (uint32_t)g_MinImageCount;
    init_info.ImageCount      = wd->ImageCount;
    init_info.Allocator       = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info, wd->RenderPass);

    // Font upload
    {
        VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;
        VkCommandBufferBeginInfo begin_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command_buffer, &begin_info);

        ImGui_ImplVulkan_CreateFontsTexture(command_buffer);

        VkSubmitInfo end_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers    = &command_buffer;
        vkEndCommandBuffer(command_buffer);
        vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);

        vkDeviceWaitIdle(g_Device);
        ImGui_ImplVulkan_DestroyFontUploadObjects();
    }

    CrawlSession session;
    session.logs.push_back("Ready. Enter an AstroBin profile URL or username, then click Start Crawl.");

    static char target_buf[512] = "https://app.astrobin.com/u/jhayes_tucson";
    static char output_dir_buf[256] = "imgs";
    static int workers = 4;
    static int limit = 0;
    static bool save_metadata = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        if (width == 0 || height == 0) continue;

        if (g_SwapChainRebuild) {
            ImGui_ImplVulkan_SetMinImageCount((uint32_t)g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device,
                &g_MainWindowData, g_QueueFamily, g_Allocator, width, height, (uint32_t)g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::GetIO().IniFilename = nullptr;

        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("AstroBinCrawlerMain", nullptr, window_flags);

        // Header Title
        {
            ImGui::TextColored(ImVec4(0.38f, 0.72f, 1.00f, 1.00f), "ASTROBIN CRAWLER");
            ImGui::SameLine();
            ImGui::TextDisabled("|  Downloader");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Target URL Input
        ImGui::Text("Target Profile or Image:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##TargetInput", target_buf, IM_ARRAYSIZE(target_buf));

        // Quick Preset Buttons
        ImGui::TextDisabled("Presets:");
        ImGui::SameLine();
        if (ImGui::SmallButton("jhayes_tucson")) {
            strncpy(target_buf, "https://app.astrobin.com/u/jhayes_tucson", sizeof(target_buf));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("CAPastrophotography")) {
            strncpy(target_buf, "https://app.astrobin.com/u/CAPastrophotography", sizeof(target_buf));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Hash: j7a388")) {
            strncpy(target_buf, "j7a388", sizeof(target_buf));
        }

        ImGui::Spacing();

        // Primary Action Controls
        {
            bool is_running = (session.status == CrawlStatus::Running);

            if (!is_running) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.44f, 0.88f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.54f, 0.98f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.09f, 0.36f, 0.76f, 1.00f));

                if (ImGui::Button("Start Crawl", ImVec2(150, 36))) {
                    RunNativeCrawler(session, target_buf, output_dir_buf, workers, limit, save_metadata);
                }
                ImGui::PopStyleColor(3);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.22f, 0.22f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.28f, 0.28f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.16f, 0.16f, 1.00f));

                if (ImGui::Button("Stop Crawl", ImVec2(150, 36))) {
                    StopNativeCrawler(session);
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.19f, 0.26f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.26f, 0.35f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.15f, 0.21f, 1.00f));

            if (ImGui::Button("Open Folder", ImVec2(130, 36))) {
                fs::path target_out = fs::path(output_dir_buf);
                if (target_out.is_relative()) target_out = GetExecutableDirectory() / target_out;
                std::error_code ec;
                if (!fs::exists(target_out, ec)) fs::create_directories(target_out, ec);
                ShellExecuteW(nullptr, L"open", target_out.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(80, 36))) {
                session.ClearLogs();
            }

            float check_offset = ImGui::GetWindowWidth() - 130.0f;
            if (check_offset > ImGui::GetCursorPosX()) {
                ImGui::SameLine(check_offset);
            }
            ImGui::Checkbox("Auto-scroll", &session.auto_scroll);
        }

        ImGui::Spacing();

        // Optional Settings Collapsible
        if (ImGui::CollapsingHeader("Settings")) {
            ImGui::Columns(2, "MinimalSettings", false);

            ImGui::Text("Threads:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderInt("##Workers", &workers, 1, 16, "%d");

            ImGui::Text("Limit (0 = all):");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##Limit", &limit, 1, 10);
            if (limit < 0) limit = 0;

            ImGui::NextColumn();

            ImGui::Text("Folder:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##OutputDir", output_dir_buf, IM_ARRAYSIZE(output_dir_buf));

            ImGui::Spacing();
            ImGui::Checkbox("Metadata (.json)", &save_metadata);

            ImGui::Columns(1);
            ImGui::Spacing();
        }

        ImGui::Spacing();

        // Output Log Area
        ImGui::Text("Log:");
        if (ImGui::BeginChild("LogConsole", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar)) {
            std::lock_guard<std::mutex> lock(session.log_mutex);
            for (const auto& line : session.logs) {
                if (line.find("Failed") != std::string::npos || line.find("Error") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.96f, 0.38f, 0.38f, 1.00f), "%s", line.c_str());
                } else if (line.find("Finished") != std::string::npos || line.find("Saved") != std::string::npos || line.find("Downloaded") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.48f, 1.00f), "%s", line.c_str());
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }

            if (session.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        FrameRender(wd, ImGui::GetDrawData());
        FramePresent(wd);
    }

    if (session.status == CrawlStatus::Running) {
        session.stop_requested = true;
    }
    if (session.crawler_thread.joinable()) {
        session.crawler_thread.join();
    }

    vkDeviceWaitIdle(g_Device);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, wd, g_Allocator);
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}