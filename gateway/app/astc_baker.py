"""Hardware Mali ASTC artwork baker & local disk cache.

Fetches Plex, YouTube, or TMDB artwork from an explicit host allowlist,
resizes/crops/blurs using Pillow, and encodes directly into ARM Mali
0x5ca1ab13 .astc GPU texture blocks using `astcenc` (AVX2 on i5-1340PE).
The TV uploads raw .astc bytes straight to glCompressedTexImage2D with
0.0 ms CPU image decoding.
"""
from __future__ import annotations

import asyncio
import hashlib
import io
import os
import shutil
import struct
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Optional, Tuple
from urllib.parse import urlparse

import aiohttp
from PIL import Image, ImageEnhance, ImageFilter, ImageOps

from .config import GatewayConfig

ASTC_MAGIC = 0x5CA1AB13

ALLOWED_REMOTE_HOSTS = frozenset(
    {
        "i.ytimg.com",
        "i9.ytimg.com",
        "img.youtube.com",
        "yt3.ggpht.com",
        "yt3.googleusercontent.com",
        "lh3.googleusercontent.com",
        "image.tmdb.org",
    }
)


@dataclass(frozen=True)
class ArtPreset:
    name: str
    width: int
    height: int
    block: str
    blur_radius: float = 0.0
    brightness: float = 1.0
    quality_flag: str = "-fast"


PRESETS: Dict[str, ArtPreset] = {
    "poster": ArtPreset("poster", 240, 360, "6x6", quality_flag="-fast"),
    "thumb": ArtPreset("thumb", 384, 216, "6x6", quality_flag="-fast"),
    "backdrop": ArtPreset("backdrop", 640, 360, "6x6", blur_radius=12.0, brightness=0.38, quality_flag="-fast"),
    "avatar": ArtPreset("avatar", 96, 96, "4x4", quality_flag="-fast"),
    "channel": ArtPreset("channel", 240, 240, "6x6", quality_flag="-fast"),
    "card_hi": ArtPreset("card_hi", 300, 450, "4x4", quality_flag="-fast"),
}


def validate_astc_bytes(data: bytes) -> Tuple[int, int, int, int]:
    """Validate a standard 16-byte .astc header and return (w, h, bx, by)."""
    if len(data) < 16:
        raise ValueError("ASTC payload shorter than 16-byte header")
    magic, bx, by, bz = struct.unpack("<IBBB", data[:7])
    if magic != ASTC_MAGIC:
        raise ValueError(f"Invalid ASTC magic: 0x{magic:08x}")
    if bz != 1:
        raise ValueError(f"Unsupported 3D ASTC block z={bz}")
    w = data[7] | (data[8] << 8) | (data[9] << 16)
    h = data[10] | (data[11] << 8) | (data[12] << 16)
    expected_blocks = ((w + bx - 1) // bx) * ((h + by - 1) // by)
    expected_size = 16 + expected_blocks * 16
    if len(data) != expected_size:
        raise ValueError(f"ASTC size mismatch: got {len(data)}, expected {expected_size}")
    return w, h, bx, by


def is_allowed_source(src: str) -> bool:
    """Prevent SSRF by allowing only Plex relative paths or whitelisted media CDNs."""
    if not src or len(src) > 2048:
        return False
    if src.startswith("/"):
        # Relative Plex path (must not be protocol-relative //evil)
        if src.startswith("//") or ".." in src:
            return False
        return src.startswith(("/library/", "/photo/", "/:/"))
    parsed = urlparse(src)
    if parsed.scheme not in ("https", "http"):
        return False
    host = (parsed.hostname or "").lower()
    return host in ALLOWED_REMOTE_HOSTS


def _prepare_image_bytes(raw: bytes, preset: ArtPreset, fmt: str) -> bytes:
    img = Image.open(io.BytesIO(raw))
    img = ImageOps.exif_transpose(img)
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    img = ImageOps.fit(img, (preset.width, preset.height), method=Image.Resampling.LANCZOS)
    if preset.blur_radius > 0:
        img = img.filter(ImageFilter.GaussianBlur(radius=preset.blur_radius))
    if preset.brightness != 1.0:
        img = ImageEnhance.Brightness(img).enhance(preset.brightness)
    buf = io.BytesIO()
    if fmt == "png":
        img.save(buf, format="PNG", compress_level=1)
    elif fmt == "webp":
        img.convert("RGB").save(buf, format="WEBP", quality=82, method=4)
    elif fmt == "jpg":
        img.convert("RGB").save(buf, format="JPEG", quality=84, optimize=True)
    else:
        raise ValueError(f"Unsupported intermediate format: {fmt}")
    return buf.getvalue()


class AstcBaker:
    def __init__(self, cfg: GatewayConfig, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.session = session
        self.astcenc_bin = shutil.which("astcenc") or shutil.which("astcenc-avx2") or ""
        self._locks: Dict[str, asyncio.Lock] = {}
        self._hits = 0
        self._misses = 0
        self._last_prune = 0.0

    def stats(self) -> dict:
        cache_dir = self.cfg.astc_cache_dir
        files = 0
        total_bytes = 0
        if cache_dir.exists():
            for p in cache_dir.rglob("*"):
                if p.is_file():
                    files += 1
                    try:
                        total_bytes += p.stat().st_size
                    except OSError:
                        pass
        return {
            "astcenc_available": bool(self.astcenc_bin),
            "astcenc_path": self.astcenc_bin,
            "cache_files": files,
            "cache_mib": round(total_bytes / (1024 * 1024), 2),
            "hits": self._hits,
            "misses": self._misses,
        }

    def _cache_key(self, preset: str, src: str, fmt: str, yflip: bool) -> str:
        raw = f"v1|{preset}|{fmt}|{int(yflip)}|{src}".encode("utf-8")
        return hashlib.sha256(raw).hexdigest()

    def _cache_path(self, key: str, fmt: str) -> Path:
        sub = self.cfg.astc_cache_dir / key[:2]
        sub.mkdir(parents=True, exist_ok=True)
        return sub / f"{key}.{fmt}"

    async def _fetch_source(self, src: str, preset: ArtPreset) -> bytes:
        if src.startswith("/"):
            # Ask Plex to pre-scale slightly above target for ultra-fast transfer from loopback
            req_w = min(preset.width * 2, 1280)
            req_h = min(preset.height * 2, 720)
            headers = {"X-Plex-Token": self.cfg.plex_token} if self.cfg.plex_token else {}
            url = (
                f"{self.cfg.plex_url}/photo/:/transcode"
                f"?width={req_w}&height={req_h}&minSize=1&upscale=1&url={src}"
            )
            async with self.session.get(url, headers=headers, timeout=aiohttp.ClientTimeout(total=12)) as r:
                if r.status == 200:
                    return await r.read()
            # Direct fallback if transcode endpoint refused the path
            direct_url = f"{self.cfg.plex_url}{src}"
            async with self.session.get(direct_url, headers=headers, timeout=aiohttp.ClientTimeout(total=12)) as r:
                r.raise_for_status()
                return await r.read()

        async with self.session.get(
            src,
            headers={"User-Agent": "Mozilla/5.0 (Linux; Tizen 9.0) LivingRoomHub/1.0"},
            timeout=aiohttp.ClientTimeout(total=12),
        ) as r:
            r.raise_for_status()
            return await r.read()

    async def _encode_astc(self, png_bytes: bytes, preset: ArtPreset, yflip: bool) -> bytes:
        if not self.astcenc_bin:
            raise RuntimeError("astcenc binary not installed on host")
        with tempfile.TemporaryDirectory(prefix="astcbake_") as tmp:
            in_path = os.path.join(tmp, "in.png")
            out_path = os.path.join(tmp, "out.astc")
            with open(in_path, "wb") as f:
                f.write(png_bytes)
            cmd = [self.astcenc_bin, "-cl", in_path, out_path, preset.block, preset.quality_flag, "-silent"]
            if yflip:
                cmd.append("-yflip")
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            _, stderr = await proc.communicate()
            if proc.returncode != 0:
                raise RuntimeError(f"astcenc failed ({proc.returncode}): {stderr.decode('utf-8', 'replace')[:300]}")
            data = Path(out_path).read_bytes()
            validate_astc_bytes(data)
            return data

    async def get_artwork(
        self,
        preset_name: str,
        src: str,
        fmt: str = "astc",
        yflip: bool = False,
    ) -> Tuple[bytes, str, bool]:
        """Return (payload_bytes, content_type, cache_hit)."""
        preset = PRESETS.get(preset_name)
        if preset is None:
            raise ValueError(f"Unknown preset: {preset_name}")
        if fmt not in ("astc", "webp", "jpg", "png"):
            raise ValueError(f"Unsupported format: {fmt}")
        if not is_allowed_source(src):
            raise PermissionError("Artwork source URL not in allowlist")

        key = self._cache_key(preset_name, src, fmt, yflip)
        path = self._cache_path(key, fmt)
        ctype = {
            "astc": "application/octet-stream",
            "webp": "image/webp",
            "jpg": "image/jpeg",
            "png": "image/png",
        }[fmt]

        if path.is_file():
            self._hits += 1
            try:
                os.utime(path, None)
            except OSError:
                pass
            return path.read_bytes(), ctype, True

        lock = self._locks.setdefault(key, asyncio.Lock())
        async with lock:
            if path.is_file():
                self._hits += 1
                return path.read_bytes(), ctype, True

            self._misses += 1
            raw = await self._fetch_source(src, preset)
            loop = asyncio.get_running_loop()
            if fmt == "astc":
                png_bytes = await loop.run_in_executor(None, _prepare_image_bytes, raw, preset, "png")
                payload = await self._encode_astc(png_bytes, preset, yflip)
            else:
                payload = await loop.run_in_executor(None, _prepare_image_bytes, raw, preset, fmt)

            tmp_path = path.with_suffix(f".{fmt}.tmp.{os.getpid()}")
            tmp_path.write_bytes(payload)
            os.replace(tmp_path, path)

            now = time.monotonic()
            if now - self._last_prune > 600:
                self._last_prune = now
                asyncio.create_task(self._prune_if_needed())

            return payload, ctype, False

    async def _prune_if_needed(self) -> None:
        max_bytes = self.cfg.astc_max_cache_mb * 1024 * 1024
        cache_dir = self.cfg.astc_cache_dir
        if not cache_dir.exists():
            return
        entries = []
        total = 0
        for p in cache_dir.rglob("*"):
            if p.is_file() and not p.name.endswith(".tmp"):
                try:
                    st = p.stat()
                    total += st.st_size
                    entries.append((st.st_mtime, st.st_size, p))
                except OSError:
                    pass
        if total <= max_bytes:
            return
        entries.sort(key=lambda x: x[0])
        target = int(max_bytes * 0.85)
        for _, sz, p in entries:
            if total <= target:
                break
            try:
                p.unlink()
                total -= sz
            except OSError:
                pass
