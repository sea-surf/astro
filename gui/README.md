# AstroBin Crawler GUI 🌌

A modern, minimalistic, **100% standalone** desktop application for downloading full-resolution astrophotography from [AstroBin](https://app.astrobin.com). Built with C++17, ImGui, Vulkan, and native Windows HTTP (WinHTTP).

## Features

- **100% Standalone Executable**: No Python, no virtual environments, and no external scripts required. You can move `GUI.exe` anywhere (Desktop, USB drive, another computer) and it works out of the box!
- **Native High-Speed Engine**: Communicates directly with AstroBin REST APIs and CDN endpoints using native Windows HTTP streaming (`WinHTTP`).
- **Concurrent Multithreaded Downloads**: Download multiple full-resolution images simultaneously with configurable worker threads.
- **Smart Target Detection**: Paste any user profile URL (`https://app.astrobin.com/u/...`), username (`@jhayes_tucson`), image URL (`https://app.astrobin.com/i/...`), or 6-character hash (`j7a388`).
- **Resilient & Resumable**: Automatically skips files that have already been downloaded.
- **Companion Technical Metadata**: Option to export companion `.json` files containing camera, telescope, filter, and exposure details.
- **Clean Minimalist Interface**: Pure, distraction-free interface with real-time live log output, quick presets, and 1-click "Open Folder" in Windows File Explorer.

## Building & Running

### Requirements (Only if compiling from source)
- [CMake 3.10+](https://cmake.org/download/)
- Visual Studio with C++ Desktop Development (MSVC C++17)
- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)

### Build Release (Standalone Binary)
```powershell
cd gui
cmake -B build -S .
cmake --build build --config Release
```

The standalone executable will be at `gui/build/Release/GUI.exe` (or `gui/GUI.exe`).
