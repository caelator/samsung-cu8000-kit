"""Unified subtitle parser & normalizer for Plex SRT/ASS and YouTube VTT/JSON3.

Produces one ultra-compact cue format for the bare-metal TV FreeType renderer:
    [[start_ms, end_ms, "clean utf-8 text"], ...]
Zero XML/HTML/ASS parsing is ever needed on the 1.15 GHz ARMv7 TV.
"""
from __future__ import annotations

import asyncio
import html
import json
import os
import re
import shutil
from pathlib import Path
from typing import Any, List, Tuple

Cue = Tuple[int, int, str]

_TAG_RE = re.compile(r"<[^>]+>")
_ASS_OVERRIDE_RE = re.compile(r"\{[^}]*\}")
_SRT_TIME_RE = re.compile(
    r"(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2}):(\d{2})[,.](\d{1,3})"
)
_VTT_SHORT_TIME_RE = re.compile(
    r"(\d{1,2}):(\d{2})[.,](\d{1,3})\s*-->\s*(\d{1,2}):(\d{2})[.,](\d{1,3})"
)


def _to_ms(h: str, m: str, s: str, ms: str) -> int:
    ms_val = int(ms.ljust(3, "0")[:3])
    return ((int(h) * 3600) + (int(m) * 60) + int(s)) * 1000 + ms_val


def clean_caption_text(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = text.replace("\\N", "\n").replace("\\n", "\n").replace("\\h", " ")
    text = _ASS_OVERRIDE_RE.sub("", text)
    text = _TAG_RE.sub("", text)
    text = html.unescape(text)
    lines = [re.sub(r"[ \t]+", " ", ln).strip() for ln in text.split("\n")]
    lines = [ln for ln in lines if ln]
    return "\n".join(lines)


def _normalize_cues(cues: List[Cue]) -> List[List[Any]]:
    cleaned: List[Cue] = []
    for start, end, text in cues:
        txt = clean_caption_text(text)
        if not txt or end <= start:
            continue
        cleaned.append((max(0, int(start)), int(end), txt))
    cleaned.sort(key=lambda c: (c[0], c[1]))

    # Deduplicate adjacent identical text (common in YouTube rolling auto-captions)
    merged: List[List[Any]] = []
    for start, end, txt in cleaned:
        if merged and merged[-1][2] == txt and start <= merged[-1][1] + 150:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end, txt])
    return merged


def parse_srt(raw: str) -> List[List[Any]]:
    raw = raw.lstrip("\ufeff").replace("\r\n", "\n").replace("\r", "\n")
    cues: List[Cue] = []
    for block in re.split(r"\n{2,}", raw.strip()):
        lines = [ln for ln in block.split("\n") if ln.strip()]
        if not lines:
            continue
        t_idx = -1
        m = None
        for idx, ln in enumerate(lines[:2]):
            m = _SRT_TIME_RE.search(ln)
            if m:
                t_idx = idx
                break
        if not m or t_idx < 0:
            continue
        start = _to_ms(m.group(1), m.group(2), m.group(3), m.group(4))
        end = _to_ms(m.group(5), m.group(6), m.group(7), m.group(8))
        text = "\n".join(lines[t_idx + 1 :])
        cues.append((start, end, text))
    return _normalize_cues(cues)


def parse_vtt(raw: str) -> List[List[Any]]:
    raw = raw.lstrip("\ufeff").replace("\r\n", "\n").replace("\r", "\n")
    cues: List[Cue] = []
    for block in re.split(r"\n{2,}", raw.strip()):
        lines = [ln for ln in block.split("\n") if ln.strip()]
        if not lines or lines[0].startswith(("WEBVTT", "NOTE", "STYLE", "REGION")):
            continue
        t_idx = -1
        start = end = 0
        for idx, ln in enumerate(lines[:2]):
            m = _SRT_TIME_RE.search(ln)
            if m:
                start = _to_ms(m.group(1), m.group(2), m.group(3), m.group(4))
                end = _to_ms(m.group(5), m.group(6), m.group(7), m.group(8))
                t_idx = idx
                break
            m2 = _VTT_SHORT_TIME_RE.search(ln)
            if m2:
                start = _to_ms("0", m2.group(1), m2.group(2), m2.group(3))
                end = _to_ms("0", m2.group(4), m2.group(5), m2.group(6))
                t_idx = idx
                break
        if t_idx < 0:
            continue
        text = "\n".join(lines[t_idx + 1 :])
        cues.append((start, end, text))
    return _normalize_cues(cues)


def parse_ass(raw: str) -> List[List[Any]]:
    raw = raw.lstrip("\ufeff").replace("\r\n", "\n").replace("\r", "\n")
    cues: List[Cue] = []
    for ln in raw.split("\n"):
        if not ln.startswith("Dialogue:"):
            continue
        parts = ln.split(",", 9)
        if len(parts) < 10:
            continue
        sm = re.match(r"(\d+):(\d{2}):(\d{2})\.(\d{1,3})", parts[1].strip())
        em = re.match(r"(\d+):(\d{2}):(\d{2})\.(\d{1,3})", parts[2].strip())
        if not sm or not em:
            continue
        # ASS uses centiseconds (2 digits)
        s_ms = int(sm.group(4).ljust(2, "0")[:2]) * 10
        e_ms = int(em.group(4).ljust(2, "0")[:2]) * 10
        start = ((int(sm.group(1)) * 3600) + (int(sm.group(2)) * 60) + int(sm.group(3))) * 1000 + s_ms
        end = ((int(em.group(1)) * 3600) + (int(em.group(2)) * 60) + int(em.group(3))) * 1000 + e_ms
        cues.append((start, end, parts[9]))
    return _normalize_cues(cues)


def parse_youtube_json3(raw: str) -> List[List[Any]]:
    data = json.loads(raw)
    cues: List[Cue] = []
    for ev in data.get("events", []):
        segs = ev.get("segs")
        if not segs:
            continue
        start = int(ev.get("tStartMs", 0))
        dur = int(ev.get("dDurationMs", 0))
        if dur <= 0:
            dur = 2500
        text = "".join(str(s.get("utf8", "")) for s in segs)
        cues.append((start, start + dur, text))
    return _normalize_cues(cues)


def parse_any_subtitle(raw: str, codec_hint: str = "") -> List[List[Any]]:
    hint = (codec_hint or "").lower()
    stripped = raw.lstrip("\ufeff \t\r\n")
    if hint == "json3" or stripped.startswith("{"):
        return parse_youtube_json3(raw)
    if hint in ("ass", "ssa") or "[Events]" in raw[:4096]:
        return parse_ass(raw)
    if hint in ("vtt", "webvtt") or stripped.startswith("WEBVTT"):
        return parse_vtt(raw)
    return parse_srt(raw)


async def extract_embedded_subtitle_srt(media_file: Path, stream_index: int) -> str:
    """Extract an embedded text subtitle stream from a local MKV/MP4 file via ffmpeg."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg binary not installed")
    if not media_file.is_file():
        raise FileNotFoundError(f"Media file not accessible: {media_file}")
    cmd = [
        ffmpeg,
        "-nostdin",
        "-v",
        "error",
        "-i",
        str(media_file),
        "-map",
        f"0:{stream_index}",
        "-f",
        "srt",
        "pipe:1",
    ]
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await proc.communicate()
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg subtitle extract failed: {stderr.decode('utf-8', 'replace')[:300]}")
    return stdout.decode("utf-8", errors="replace")


def load_cached_cues(cache_dir: Path, cache_key: str) -> dict | None:
    p = cache_dir / f"{cache_key}.json"
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def save_cached_cues(cache_dir: Path, cache_key: str, payload: dict) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    p = cache_dir / f"{cache_key}.json"
    tmp = p.with_suffix(f".tmp.{os.getpid()}")
    tmp.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")
    os.replace(tmp, p)
