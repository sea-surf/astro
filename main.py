"""
AstroBin Full-Resolution Image Crawler & Downloader
Downloads full-resolution astrophotography images from app.astrobin.com.
Supports user profiles (full galleries recursively) and individual image links.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Generator, Optional
from urllib.parse import parse_qs, urlparse

import httpx
from rich.console import Console
from rich.panel import Panel
from rich.progress import (
    BarColumn,
    DownloadColumn,
    Progress,
    SpinnerColumn,
    TaskID,
    TextColumn,
    TimeRemainingColumn,
    TransferSpeedColumn,
)
from rich.table import Table

console = Console()

# Standard headers to mimic a modern browser and avoid bot detection
DEFAULT_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
    ),
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "en-US,en;q=0.9",
    "Origin": "https://app.astrobin.com",
    "Referer": "https://app.astrobin.com/",
}


def sanitize_filename(name: str, max_length: int = 120) -> str:
    """Sanitizes strings for safe cross-platform filesystem filenames."""
    cleaned = re.sub(r'[\\/*?:"<>|]', "", name)
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    if len(cleaned) > max_length:
        cleaned = cleaned[:max_length].rstrip()
    return cleaned or "unnamed"


class AstroBinClient:
    """Client for interacting with AstroBin APIs and downloading images."""

    BASE_APP_API = "https://app.astrobin.com/api/v2"
    BASE_WWW = "https://www.astrobin.com"

    def __init__(self, delay: float = 0.1, timeout: float = 30.0, max_connections: int = 10):
        self.delay = delay
        self.timeout = timeout
        limits = httpx.Limits(max_connections=max_connections, max_keepalive_connections=max_connections)
        self.session = httpx.Client(
            headers=DEFAULT_HEADERS,
            timeout=self.timeout,
            limits=limits,
            follow_redirects=True,
        )
        self._lock = threading.Lock()

    def close(self):
        self.session.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _get(self, url: str, params: Optional[dict[str, Any]] = None, retries: int = 3) -> httpx.Response:
        """Performs a GET request with automatic retry and rate-limiting."""
        if self.delay > 0:
            time.sleep(self.delay)

        last_error = None
        for attempt in range(retries):
            try:
                resp = self.session.get(url, params=params)
                if resp.status_code == 429:
                    wait_time = 2.0 * (attempt + 1)
                    console.print(f"[yellow]Rate limited (429). Backing off for {wait_time:.1f}s...[/yellow]")
                    time.sleep(wait_time)
                    continue
                resp.raise_for_status()
                return resp
            except (httpx.RequestError, httpx.HTTPStatusError) as e:
                last_error = e
                if attempt < retries - 1:
                    time.sleep(1.0 * (attempt + 1))
                else:
                    raise last_error
        raise last_error or RuntimeError("Request failed")

    def get_user_by_username(self, username: str) -> Optional[dict[str, Any]]:
        """Resolves an AstroBin username into user details (including numeric ID)."""
        url = f"{self.BASE_APP_API}/common/users/?username={username}"
        try:
            resp = self._get(url)
            data = resp.json()
            if isinstance(data, list) and len(data) > 0:
                return data[0]
        except Exception as e:
            console.print(f"[red]Error fetching user '{username}': {e}[/red]")
        return None

    def get_image_details(self, hash_or_id: str) -> Optional[dict[str, Any]]:
        """Fetches complete metadata for a single image by its hash or ID."""
        url = f"{self.BASE_APP_API}/images/image/?hash={hash_or_id}"
        try:
            resp = self._get(url)
            data = resp.json()
            results = data.get("results", [])
            if results:
                return results[0]
        except Exception as e:
            console.print(f"[red]Error fetching image details for '{hash_or_id}': {e}[/red]")
        return None

    def get_user_gallery_page(self, user_id: int, page: int = 1) -> dict[str, Any]:
        """Fetches one page of a user's gallery using the gallery serializer."""
        url = f"{self.BASE_APP_API}/images/image/"
        params = {
            "user": user_id,
            "gallery-serializer": "1",
            "page": page,
        }
        resp = self._get(url, params=params)
        return resp.json()

    def iter_user_images(self, user_id: int, limit: Optional[int] = None) -> Generator[dict[str, Any], None, None]:
        """Recursively yields all images in a user's gallery across all pages."""
        page = 1
        yielded_count = 0

        while True:
            data = self.get_user_gallery_page(user_id, page=page)
            results = data.get("results", [])
            if not results:
                break

            for item in results:
                yield item
                yielded_count += 1
                if limit and yielded_count >= limit:
                    return

            if not data.get("next"):
                break

            page += 1

    def get_thumbnail_url(self, image_hash: str, revision: str = "0", alias: str = "real") -> Optional[str]:
        """
        Queries AstroBin's classic thumbnail endpoint for a specific alias.
        Aliases: 'real' (full resolution original), 'qhd' (2560px), 'hd' (1824px), 'regular' (620px).
        """
        url = f"{self.BASE_WWW}/{image_hash}/{revision}/thumb/{alias}/"
        try:
            resp = self._get(url)
            data = resp.json()
            target_url = data.get("url")
            if target_url and "placeholder" not in target_url and "loading.gif" not in target_url:
                return target_url
        except Exception:
            pass
        return None

    def get_best_image_url(self, image_hash: str, revision: str = "0") -> tuple[str, str]:
        """
        Attempts to find the highest resolution image URL for an image.
        Tries 'real' (full resolution 32768x0) first, then falls back to 'qhd', 'hd', 'regular'.
        """
        for alias in ["real", "qhd", "hd", "regular"]:
            url = self.get_thumbnail_url(image_hash, revision=revision, alias=alias)
            if url:
                return url, alias
        raise RuntimeError(f"Could not retrieve any image URL for {image_hash} (revision: {revision})")

    def download_file(
        self,
        url: str,
        dest_path: Path,
        progress: Optional[Progress] = None,
        task_id: Optional[TaskID] = None,
        overwrite: bool = False,
    ) -> bool:
        """
        Streams a file from URL to dest_path with resume capability.
        Returns True if downloaded, False if skipped because it already exists.
        """
        dest_path.parent.mkdir(parents=True, exist_ok=True)

        # Check existing file for resume
        if dest_path.exists() and not overwrite:
            try:
                head_resp = self.session.head(url)
                remote_size = int(head_resp.headers.get("content-length", 0))
                local_size = dest_path.stat().st_size
                if remote_size > 0 and local_size == remote_size:
                    if progress and task_id is not None:
                        progress.update(task_id, completed=local_size, total=local_size)
                    return False
            except Exception:
                pass

        temp_path = dest_path.with_suffix(dest_path.suffix + ".part")
        with self.session.stream("GET", url) as resp:
            resp.raise_for_status()
            total_size = int(resp.headers.get("content-length", 0))
            if progress and task_id is not None:
                progress.update(task_id, total=total_size, completed=0)

            with open(temp_path, "wb") as f:
                for chunk in resp.iter_bytes(chunk_size=65536):
                    f.write(chunk)
                    if progress and task_id is not None:
                        progress.update(task_id, advance=len(chunk))

        if temp_path.exists():
            # Windows file locking resilience (antivirus, indexer)
            for attempt in range(5):
                try:
                    if dest_path.exists():
                        dest_path.unlink(missing_ok=True)
                    temp_path.replace(dest_path)
                    break
                except OSError:
                    time.sleep(0.1 * (attempt + 1))
            else:
                import shutil
                shutil.move(str(temp_path), str(dest_path))
        return True


def parse_target_input(target: str) -> dict[str, Any]:
    """Parses any target input into an actionable dictionary."""
    target = target.strip()
    parsed = urlparse(target)

    if not parsed.scheme:
        if re.match(r"^[a-z0-9]{5,8}$", target, re.IGNORECASE) or target.isdigit():
            return {"mode": "single_image", "hash": target, "username": None}
        return {"mode": "user_gallery", "username": target}

    qs = parse_qs(parsed.query)

    if "i" in qs:
        image_hash = qs["i"][0]
        user_match = re.search(r"/(?:u|users)/([^/?#]+)", parsed.path)
        username = user_match.group(1) if user_match else None
        return {
            "mode": "single_image_with_user",
            "hash": image_hash,
            "username": username,
        }

    m_img = re.search(r"/(?:i|image)/([a-z0-9]+)", parsed.path, re.IGNORECASE)
    if m_img:
        return {"mode": "single_image", "hash": m_img.group(1), "username": None}

    m_hash = re.search(r"^/([a-z0-9]+)/?$", parsed.path, re.IGNORECASE)
    if m_hash:
        return {"mode": "single_image", "hash": m_hash.group(1), "username": None}

    m_user = re.search(r"/(?:u|users)/([^/?#]+)", parsed.path)
    if m_user:
        return {"mode": "user_gallery", "username": m_user.group(1)}

    return {"mode": "user_gallery", "username": target}


def crawl_user_gallery(
    client: AstroBinClient,
    username: str,
    output_dir: Path,
    limit: Optional[int] = None,
    workers: int = 4,
    include_revisions: bool = False,
    save_metadata: bool = False,
    overwrite: bool = False,
):
    """Recursively crawls and downloads all images for a given user concurrently."""
    console.print(f"[bold cyan]Looking up user:[/bold cyan] {username}...")
    user_info = client.get_user_by_username(username)
    if not user_info:
        console.print(f"[bold red]User '{username}' not found on AstroBin![/bold red]")
        return

    user_id = user_info["id"]
    display_name = user_info.get("display_name") or username
    console.print(
        Panel.fit(
            f"[bold green]User Found![/bold green]\n"
            f"[white]Display Name:[/white] {display_name}\n"
            f"[white]User ID:[/white] {user_id}\n"
            f"[white]Subscription:[/white] {user_info.get('valid_subscription', 'Standard')}",
            title="AstroBin User Profile",
            border_style="cyan",
        )
    )

    user_dir = output_dir / sanitize_filename(username)
    user_dir.mkdir(parents=True, exist_ok=True)

    console.print("[cyan]Scanning user gallery across all pages...[/cyan]")
    first_page = client.get_user_gallery_page(user_id, page=1)
    total_in_profile = first_page.get("count", 0)
    console.print(f"[green]Total images in gallery:[/green] [bold]{total_in_profile}[/bold]")

    # Gather all items to download
    items = list(client.iter_user_images(user_id, limit=limit))
    target_count = len(items)

    if target_count == 0:
        console.print("[yellow]No images found for this user.[/yellow]")
        return

    console.print(
        f"[bold]Starting download of {target_count} image(s) using {workers} parallel worker(s)...[/bold]\n"
    )

    overall_progress = Progress(
        SpinnerColumn(),
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
        TextColumn("({task.completed}/{task.total})"),
        console=console,
    )

    stats = {"downloaded": 0, "skipped": 0, "failed": 0}
    stats_lock = threading.Lock()

    def process_item(item: dict[str, Any]):
        img_identifier = item.get("hash") or str(item.get("pk"))
        img_title = item.get("title") or "untitled"

        revisions_to_download = [("0", "original")]
        if include_revisions and item.get("revisions"):
            for rev in item["revisions"]:
                rev_label = rev.get("label")
                if rev_label:
                    revisions_to_download.append((rev_label, f"rev_{rev_label}"))

        for rev_id, rev_tag in revisions_to_download:
            suffix = f"_{rev_tag}" if rev_tag != "original" else ""
            clean_title = sanitize_filename(img_title)
            filename = f"{img_identifier}{suffix}_{clean_title}.jpg"
            file_path = user_dir / filename

            try:
                url, alias_used = client.get_best_image_url(img_identifier, revision=rev_id)
                downloaded = client.download_file(
                    url=url,
                    dest_path=file_path,
                    overwrite=overwrite,
                )

                with stats_lock:
                    if downloaded:
                        stats["downloaded"] += 1
                        console.print(f"  [green]Downloaded:[/green] {filename} [dim]({alias_used})[/dim]")
                    else:
                        stats["skipped"] += 1
                        console.print(f"  [yellow]Skipped (exists):[/yellow] {filename}")

                if save_metadata:
                    meta_path = file_path.with_suffix(".json")
                    with open(meta_path, "w", encoding="utf-8") as mf:
                        json.dump(item, mf, indent=2, ensure_ascii=False)

            except Exception as e:
                with stats_lock:
                    stats["failed"] += 1
                    console.print(f"  [red]Failed {img_identifier} ({filename}): {e}[/red]")

        overall_progress.advance(overall_task)

    with overall_progress:
        overall_task = overall_progress.add_task("[bold green]Total Progress", total=target_count)

        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = [executor.submit(process_item, item) for item in items]
            for future in as_completed(futures):
                try:
                    future.result()
                except Exception as e:
                    console.print(f"[red]Worker error: {e}[/red]")

    table = Table(title="Download Summary", border_style="green")
    table.add_column("Status", style="cyan")
    table.add_column("Count", style="bold white")
    table.add_row("Total In Gallery", str(total_in_profile))
    table.add_row("Successfully Downloaded", str(stats["downloaded"]))
    table.add_row("Skipped (Already Exists)", str(stats["skipped"]))
    table.add_row("Failed", str(stats["failed"]))
    table.add_row("Output Directory", str(user_dir.resolve()))
    console.print(table)


def crawl_single_image(
    client: AstroBinClient,
    image_hash: str,
    output_dir: Path,
    username_hint: Optional[str] = None,
    include_revisions: bool = False,
    save_metadata: bool = False,
    overwrite: bool = False,
):
    """Downloads a single image (and optionally its revisions) by hash."""
    console.print(f"[bold cyan]Fetching metadata for image:[/bold cyan] {image_hash}...")
    details = client.get_image_details(image_hash)

    title = "untitled"
    username = username_hint or "astrobin_images"
    w = 0
    h = 0

    if details:
        title = details.get("title") or title
        username = details.get("username") or username
        w = details.get("w", 0)
        h = details.get("h", 0)

    dest_dir = output_dir / sanitize_filename(username)
    dest_dir.mkdir(parents=True, exist_ok=True)

    console.print(
        Panel.fit(
            f"[bold green]Image Details:[/bold green]\n"
            f"[white]Title:[/white] {title}\n"
            f"[white]Author:[/white] {username}\n"
            f"[white]Resolution:[/white] {w}x{h}\n"
            f"[white]Hash:[/white] {image_hash}",
            title="AstroBin Image",
            border_style="cyan",
        )
    )

    revisions_to_download = [("0", "original")]
    if include_revisions and details and details.get("revisions"):
        for rev in details["revisions"]:
            rev_label = rev.get("label")
            if rev_label:
                revisions_to_download.append((rev_label, f"rev_{rev_label}"))

    progress = Progress(
        TextColumn("[bold blue]{task.fields[filename]}"),
        BarColumn(),
        DownloadColumn(),
        TransferSpeedColumn(),
        TimeRemainingColumn(),
        console=console,
    )

    with progress:
        for rev_id, rev_tag in revisions_to_download:
            suffix = f"_{rev_tag}" if rev_tag != "original" else ""
            clean_title = sanitize_filename(title)
            filename = f"{image_hash}{suffix}_{clean_title}.jpg"
            file_path = dest_dir / filename

            try:
                url, alias = client.get_best_image_url(image_hash, revision=rev_id)
                console.print(f"[dim]URL: {url}[/dim]")
                task_id = progress.add_task("download", filename=filename, total=0)
                downloaded = client.download_file(
                    url=url,
                    dest_path=file_path,
                    progress=progress,
                    task_id=task_id,
                    overwrite=overwrite,
                )
                progress.remove_task(task_id)

                if downloaded:
                    console.print(f"[bold green]Saved:[/bold green] {file_path}")
                else:
                    console.print(f"[yellow]Already exists (skipped):[/yellow] {file_path}")

                if save_metadata and details:
                    meta_path = file_path.with_suffix(".json")
                    with open(meta_path, "w", encoding="utf-8") as mf:
                        json.dump(details, mf, indent=2, ensure_ascii=False)

            except Exception as e:
                console.print(f"[bold red]Failed to download image {image_hash}: {e}[/bold red]")


def main():
    parser = argparse.ArgumentParser(
        description="AstroBin Full-Resolution Image Crawler & Downloader",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Download full gallery (all 180 images) from user profile:
  python main.py https://app.astrobin.com/u/jhayes_tucson

  # Download faster with 6 parallel threads:
  python main.py https://app.astrobin.com/u/jhayes_tucson --workers 6

  # Download first 10 images for testing:
  python main.py https://app.astrobin.com/u/jhayes_tucson --limit 10

  # Download a single image:
  python main.py https://app.astrobin.com/u/jhayes_tucson?i=j7a388
        """,
    )

    parser.add_argument(
        "target",
        nargs="?",
        default="https://app.astrobin.com/u/jhayes_tucson",
        help="AstroBin user URL, image URL, username, or image hash (default: https://app.astrobin.com/u/jhayes_tucson)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=Path("imgs"),
        help="Directory to save downloaded images (default: imgs/)",
    )
    parser.add_argument(
        "-w",
        "--workers",
        type=int,
        default=4,
        help="Number of concurrent download threads (default: 4)",
    )
    parser.add_argument(
        "-l",
        "--limit",
        type=int,
        default=None,
        help="Maximum number of images to download from gallery (default: unlimited)",
    )
    parser.add_argument(
        "-d",
        "--delay",
        type=float,
        default=0.1,
        help="Polite delay between requests in seconds (default: 0.1)",
    )
    parser.add_argument(
        "-r",
        "--include-revisions",
        action="store_true",
        help="Download all revisions (A, B, C...) in addition to final images",
    )
    parser.add_argument(
        "-m",
        "--save-metadata",
        action="store_true",
        help="Save image metadata as a companion .json file alongside each image",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Overwrite existing files instead of resuming/skipping",
    )
    parser.add_argument(
        "-g",
        "--gallery",
        action="store_true",
        help="Force crawling the whole user gallery even if an image URL (?i=...) was provided",
    )

    args = parser.parse_args()

    parsed = parse_target_input(args.target)

    with AstroBinClient(delay=args.delay, max_connections=args.workers * 2) as client:
        if args.gallery and parsed.get("username"):
            crawl_user_gallery(
                client=client,
                username=parsed["username"],
                output_dir=args.output_dir,
                limit=args.limit,
                workers=args.workers,
                include_revisions=args.include_revisions,
                save_metadata=args.save_metadata,
                overwrite=args.overwrite,
            )
        elif parsed["mode"] == "user_gallery":
            crawl_user_gallery(
                client=client,
                username=parsed["username"],
                output_dir=args.output_dir,
                limit=args.limit,
                workers=args.workers,
                include_revisions=args.include_revisions,
                save_metadata=args.save_metadata,
                overwrite=args.overwrite,
            )
        elif parsed["mode"] in ("single_image", "single_image_with_user"):
            crawl_single_image(
                client=client,
                image_hash=parsed["hash"],
                output_dir=args.output_dir,
                username_hint=parsed.get("username"),
                include_revisions=args.include_revisions,
                save_metadata=args.save_metadata,
                overwrite=args.overwrite,
            )


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        console.print("\n[yellow]Crawl interrupted by user (Ctrl+C).[/yellow]")
        sys.exit(0)
