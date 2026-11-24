"""Loopback Plex Media Server (1.43.5) client & CU8000/eARC playback decision engine.

Tailored for Samsung CU8000 (Tizen 9) + eARC AVR / local Plex server:
- 99.7% of library is H.264 8-bit SDR -> Direct Play (0% NAS CPU)
- AAC/AC3/EAC3 audio -> Direct Play / eARC bitstream
- ~1.6% DTS/TrueHD -> Direct Stream audio-only to EAC3 5.1 (video untouched!)
- 99.0% SRT subtitles -> Native TV GLES2/FreeType overlay (zero burn-in!)
"""
from __future__ import annotations

import hashlib
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import quote

import aiohttp

from .config import GatewayConfig
from .subtitles import (
    extract_embedded_subtitle_srt,
    load_cached_cues,
    parse_any_subtitle,
    save_cached_cues,
)

DIRECT_VIDEO_CODECS = frozenset({"h264", "hevc", "h265", "vp9", "av1"})
DIRECT_AUDIO_CODECS = frozenset({"aac", "ac3", "eac3", "mp3"})
DIRECT_CONTAINERS = frozenset({"mkv", "mp4", "mov", "mpegts", "ts"})
TEXT_SUBTITLE_CODECS = frozenset({"srt", "subrip", "ass", "ssa", "vtt", "webvtt", "mov_text", "tx3g"})
IMAGE_SUBTITLE_CODECS = frozenset({"pgs", "hdmv_pgs_subtitle", "vobsub", "dvd_subtitle", "idx"})


def decide_playback(
    media: Dict[str, Any],
    part: Dict[str, Any],
    audio_stream: Optional[Dict[str, Any]],
    sub_stream: Optional[Dict[str, Any]],
    force_transcode: bool = False,
) -> Dict[str, Any]:
    """Pure decision engine for Samsung CU8000 + eARC AVR."""
    streams = part.get("Stream") or []
    v_stream = next((s for s in streams if s.get("streamType") == 1), {})

    container = str(media.get("container") or part.get("container") or "").lower()
    vcodec = str(v_stream.get("codec") or media.get("videoCodec") or "").lower()
    bit_depth = int(v_stream.get("bitDepth") or 8)
    dovi = bool(v_stream.get("DOVIPresent"))
    dovi_bl_compat = int(v_stream.get("DOVIBLCompatID") or 0)
    color_trc = str(v_stream.get("colorTrc") or "").lower()

    acodec = str((audio_stream or {}).get("codec") or media.get("audioCodec") or "").lower()
    achannels = int((audio_stream or {}).get("channels") or media.get("audioChannels") or 2)

    sub_codec = str((sub_stream or {}).get("codec") or "").lower() if sub_stream else ""
    sub_mode = "none"
    burn_sub = False
    if sub_stream:
        if sub_codec in TEXT_SUBTITLE_CODECS:
            sub_mode = "native_cues"
        elif sub_codec in IMAGE_SUBTITLE_CODECS:
            sub_mode = "burn_in"
            burn_sub = True
        else:
            sub_mode = "native_cues"

    hdr = "SDR"
    if dovi:
        hdr = "DV+HDR10" if dovi_bl_compat in (1, 2, 4, 6) else "DV"
    elif color_trc == "smpte2084":
        hdr = "HDR10"
    elif color_trc == "arib-std-b67":
        hdr = "HLG"

    video_ok = vcodec in DIRECT_VIDEO_CODECS
    video_reasons: List[str] = []
    if vcodec == "h264" and bit_depth > 8:
        video_ok = False
        video_reasons.append("h264_10bit")
    elif dovi and dovi_bl_compat not in (1, 2, 4, 6):
        video_ok = False
        video_reasons.append("dolby_vision_no_hdr10_fallback")
    elif not video_ok:
        video_reasons.append(f"video_codec_{vcodec or 'unknown'}")

    audio_ok = acodec in DIRECT_AUDIO_CODECS
    container_ok = container in DIRECT_CONTAINERS

    if force_transcode or burn_sub or not video_ok:
        mode = "transcode_video"
        reasons = list(video_reasons)
        if burn_sub:
            reasons.append(f"bitmap_sub_{sub_codec}")
        if force_transcode:
            reasons.append("user_forced")
        reason = ",".join(reasons) or "transcode"
        target_acodec = "eac3" if achannels >= 6 else "aac"
    elif not audio_ok or not container_ok:
        mode = "direct_stream_audio"
        reasons = []
        if not audio_ok:
            reasons.append(f"audio_codec_{acodec or 'unknown'}")
        if not container_ok:
            reasons.append(f"container_{container or 'unknown'}")
        reason = ",".join(reasons)
        target_acodec = "eac3" if achannels >= 6 else "aac"
    else:
        mode = "direct_play"
        reason = "native_cu8000_earc"
        target_acodec = acodec

    return {
        "mode": mode,
        "reason": reason,
        "video_codec": vcodec,
        "video_bit_depth": bit_depth,
        "hdr": hdr,
        "width": int(v_stream.get("width") or media.get("width") or 0),
        "height": int(v_stream.get("height") or media.get("height") or 0),
        "audio_codec": acodec,
        "audio_channels": achannels,
        "target_audio_codec": target_acodec,
        "container": container,
        "sub_mode": sub_mode,
        "sub_codec": sub_codec,
    }


class PlexClient:
    def __init__(self, cfg: GatewayConfig, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.session = session
        self._sections_cache: Tuple[float, List[Dict[str, Any]]] = (0.0, [])

    def _headers(self) -> Dict[str, str]:
        h = {
            "Accept": "application/json",
            "X-Plex-Client-Identifier": self.cfg.client_identifier,
            "X-Plex-Product": self.cfg.client_product,
            "X-Plex-Version": "1.0.0",
            "X-Plex-Platform": self.cfg.client_platform,
            "X-Plex-Device": self.cfg.client_device,
        }
        if self.cfg.plex_token:
            h["X-Plex-Token"] = self.cfg.plex_token
        return h

    def _art_url(self, preset: str, plex_path: Optional[str]) -> str:
        if not plex_path:
            return ""
        return f"{self.cfg.public_base_url}/v1/art/{preset}?src={quote(plex_path, safe='')}"

    async def _get_json(self, path: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        url = f"{self.cfg.plex_url}{path}"
        async with self.session.get(
            url,
            headers=self._headers(),
            params=params,
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            r.raise_for_status()
            return await r.json(content_type=None)

    async def health(self) -> Dict[str, Any]:
        t0 = time.monotonic()
        try:
            url = f"{self.cfg.plex_url}/identity"
            async with self.session.get(url, headers=self._headers(), timeout=aiohttp.ClientTimeout(total=3)) as r:
                ok = r.status == 200
                ms = round((time.monotonic() - t0) * 1000, 1)
                return {
                    "online": ok,
                    "latency_ms": ms,
                    "has_token": bool(self.cfg.plex_token),
                    "machine_id_present": bool(self.cfg.plex_machine_id),
                }
        except Exception as exc:
            return {"online": False, "error": str(exc), "has_token": bool(self.cfg.plex_token)}

    async def get_sections(self, force: bool = False) -> List[Dict[str, Any]]:
        now = time.monotonic()
        if not force and self._sections_cache[1] and (now - self._sections_cache[0]) < 120:
            return self._sections_cache[1]
        data = await self._get_json("/library/sections")
        out = []
        for d in data.get("MediaContainer", {}).get("Directory", []):
            sid = int(d["key"])
            out.append(
                {
                    "id": sid,
                    "title": d.get("title", ""),
                    "type": d.get("type", ""),
                    "is_kids": sid in self.cfg.kids_section_ids,
                }
            )
        self._sections_cache = (now, out)
        return out

    def normalize_card(self, item: Dict[str, Any], card_art: str = "poster") -> Dict[str, Any]:
        itype = str(item.get("type") or "")
        rating_key = str(item.get("ratingKey") or "")
        duration_ms = int(item.get("duration") or 0)
        offset_ms = int(item.get("viewOffset") or 0)
        view_count = int(item.get("viewCount") or 0)
        sec_id = int(item.get("librarySectionID") or 0)

        if itype == "episode":
            show = item.get("grandparentTitle") or ""
            snum = int(item.get("parentIndex") or 0)
            enum = int(item.get("index") or 0)
            ep_title = item.get("title") or ""
            title = show or ep_title
            subtitle = f"S{snum:02d}E{enum:02d} · {ep_title}" if show else ep_title
            poster_src = item.get("grandparentThumb") or item.get("parentThumb") or item.get("thumb")
            thumb_src = item.get("thumb") or item.get("art") or poster_src
        elif itype == "show":
            title = item.get("title") or ""
            leaf = int(item.get("leafCount") or 0)
            viewed = int(item.get("viewedLeafCount") or 0)
            unwatched = max(0, leaf - viewed)
            subtitle = f"{unwatched} unwatched" if unwatched else f"{leaf} episodes"
            poster_src = item.get("thumb")
            thumb_src = item.get("art") or item.get("thumb")
        else:
            title = item.get("title") or ""
            year = item.get("year")
            mins = duration_ms // 60000 if duration_ms else 0
            parts = [str(year)] if year else []
            if mins:
                parts.append(f"{mins}m")
            subtitle = " · ".join(parts)
            poster_src = item.get("thumb")
            thumb_src = item.get("art") or item.get("thumb")

        progress = round(min(1.0, offset_ms / duration_ms), 3) if duration_ms > 0 and offset_ms > 0 else 0.0
        art_src = thumb_src if card_art == "thumb" else poster_src
        return {
            "id": f"plex:{rating_key}",
            "source": "plex",
            "rating_key": rating_key,
            "type": itype,
            "section_id": sec_id,
            "is_kids": sec_id in self.cfg.kids_section_ids,
            "title": title,
            "subtitle": subtitle,
            "year": int(item.get("year") or 0),
            "duration_ms": duration_ms,
            "offset_ms": offset_ms,
            "progress": progress,
            "watched": view_count > 0,
            "poster_astc": self._art_url("poster", poster_src),
            "thumb_astc": self._art_url("thumb", thumb_src),
            "art_astc": self._art_url(card_art, art_src),
            "backdrop_astc": self._art_url("backdrop", item.get("art") or thumb_src),
        }

    async def get_continue_watching(self, limit: int = 20, kids_only: Optional[bool] = None) -> List[Dict[str, Any]]:
        data = await self._get_json("/hubs/continueWatching/items", {"X-Plex-Container-Size": str(limit * 2)})
        items = data.get("MediaContainer", {}).get("Metadata", [])
        cards = [self.normalize_card(it, card_art="thumb") for it in items]
        if kids_only is True:
            cards = [c for c in cards if c["is_kids"]]
        elif kids_only is False:
            cards = [c for c in cards if not c["is_kids"]]
        return cards[:limit]

    async def get_recently_added(self, section_id: int, limit: int = 16, card_art: str = "poster") -> List[Dict[str, Any]]:
        data = await self._get_json(
            f"/library/sections/{section_id}/recentlyAdded",
            {"X-Plex-Container-Start": "0", "X-Plex-Container-Size": str(limit)},
        )
        items = data.get("MediaContainer", {}).get("Metadata", [])
        return [self.normalize_card(it, card_art=card_art) for it in items]

    async def get_section_items(
        self,
        section_id: int,
        start: int = 0,
        size: int = 40,
        sort: str = "addedAt:desc",
        unwatched: bool = False,
        letter: str = "",
    ) -> Dict[str, Any]:
        params: Dict[str, Any] = {
            "X-Plex-Container-Start": str(max(0, start)),
            "X-Plex-Container-Size": str(max(1, min(size, 120))),
            "sort": sort,
        }
        if unwatched:
            params["unwatched"] = "1"
        if letter:
            params["title<"] = ""  # ignored; use Plex firstCharacter endpoint if single letter
        path = f"/library/sections/{section_id}/all"
        if letter and len(letter) == 1 and letter.isalnum():
            params["title"] = letter
        data = await self._get_json(path, params)
        mc = data.get("MediaContainer", {})
        items = mc.get("Metadata", [])
        return {
            "section_id": section_id,
            "total": int(mc.get("totalSize") or len(items)),
            "start": start,
            "size": len(items),
            "items": [self.normalize_card(it, card_art="poster") for it in items],
        }

    async def get_item_detail(self, rating_key: str) -> Dict[str, Any]:
        data = await self._get_json(
            f"/library/metadata/{rating_key}",
            {"includeMarkers": "1", "includeChapters": "1", "includeOnDeck": "1"},
        )
        metas = data.get("MediaContainer", {}).get("Metadata", [])
        if not metas:
            raise KeyError(f"Plex item {rating_key} not found")
        it = metas[0]
        card = self.normalize_card(it, card_art="poster")

        markers = []
        for m in it.get("Marker") or []:
            mtype = str(m.get("type") or "").lower()
            markers.append(
                {
                    "type": mtype,
                    "start_ms": int(m.get("startTimeOffset") or 0),
                    "end_ms": int(m.get("endTimeOffset") or 0),
                    "action": "prompt_skip",
                }
            )

        chapters = [
            {
                "title": str(c.get("tag") or f"Chapter {c.get('index', '')}"),
                "start_ms": int(c.get("startTimeOffset") or 0),
                "end_ms": int(c.get("endTimeOffset") or 0),
            }
            for c in (it.get("Chapter") or [])
        ]

        audio_tracks = []
        subtitle_tracks = []
        media_info: Dict[str, Any] = {}
        medias = it.get("Media") or []
        if medias:
            m0 = medias[0]
            parts = m0.get("Part") or []
            p0 = parts[0] if parts else {}
            streams = p0.get("Stream") or []
            for s in streams:
                st = s.get("streamType")
                if st == 2:
                    audio_tracks.append(
                        {
                            "id": int(s.get("id") or 0),
                            "index": int(s.get("index") or 0),
                            "codec": str(s.get("codec") or ""),
                            "channels": int(s.get("channels") or 2),
                            "lang": str(s.get("languageCode") or s.get("language") or "und"),
                            "label": str(s.get("displayTitle") or s.get("extendedDisplayTitle") or s.get("language") or "Audio"),
                            "selected": bool(s.get("selected")),
                        }
                    )
                elif st == 3:
                    codec = str(s.get("codec") or "").lower()
                    subtitle_tracks.append(
                        {
                            "id": int(s.get("id") or 0),
                            "index": int(s.get("index") or -1),
                            "codec": codec,
                            "lang": str(s.get("languageCode") or s.get("language") or "und"),
                            "label": str(s.get("displayTitle") or s.get("extendedDisplayTitle") or s.get("language") or "Sub"),
                            "forced": bool(s.get("forced")),
                            "external": bool(s.get("key")),
                            "native_overlay": codec in TEXT_SUBTITLE_CODECS,
                            "selected": bool(s.get("selected")),
                        }
                    )
            sel_audio = next((a for a in audio_tracks if a["selected"]), audio_tracks[0] if audio_tracks else None)
            sel_sub = next((s for s in subtitle_tracks if s["selected"]), None)
            raw_audio = next((s for s in streams if s.get("streamType") == 2 and s.get("id") == (sel_audio["id"] if sel_audio else -1)), None)
            raw_sub = next((s for s in streams if s.get("streamType") == 3 and s.get("id") == (sel_sub["id"] if sel_sub else -1)), None)
            media_info = decide_playback(m0, p0, raw_audio, raw_sub)

        children: List[Dict[str, Any]] = []
        if card["type"] in ("show", "season"):
            children = await self.get_children(rating_key)

        return {
            **card,
            "summary": str(it.get("summary") or ""),
            "tagline": str(it.get("tagline") or ""),
            "content_rating": str(it.get("contentRating") or ""),
            "rating": float(it.get("audienceRating") or it.get("rating") or 0.0),
            "genres": [str(g.get("tag")) for g in (it.get("Genre") or [])[:5] if g.get("tag")],
            "cast": [str(r.get("tag")) for r in (it.get("Role") or [])[:6] if r.get("tag")],
            "parent_key": str(it.get("parentRatingKey") or ""),
            "grandparent_key": str(it.get("grandparentRatingKey") or ""),
            "media": media_info,
            "audio_tracks": audio_tracks,
            "subtitle_tracks": subtitle_tracks,
            "markers": markers,
            "chapters": chapters,
            "children": children,
        }

    async def get_children(self, rating_key: str) -> List[Dict[str, Any]]:
        data = await self._get_json(f"/library/metadata/{rating_key}/children")
        items = data.get("MediaContainer", {}).get("Metadata", [])
        out = []
        for it in items:
            itype = str(it.get("type") or "")
            card = self.normalize_card(it, card_art="thumb" if itype == "episode" else "poster")
            card["index"] = int(it.get("index") or 0)
            card["parent_index"] = int(it.get("parentIndex") or 0)
            card["summary"] = str(it.get("summary") or "")
            out.append(card)
        return out

    async def _find_up_next(self, item: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if item.get("type") != "episode":
            return None
        season_key = str(item.get("parentRatingKey") or "")
        cur_idx = int(item.get("index") or 0)
        if not season_key:
            return None
        try:
            eps = await self.get_children(season_key)
            for ep in eps:
                if ep["index"] == cur_idx + 1:
                    return ep
        except Exception:
            return None
        return None

    async def resolve_playback(
        self,
        rating_key: str,
        audio_stream_id: Optional[int] = None,
        sub_stream_id: Optional[int] = None,
        force_transcode: bool = False,
        start_ms: int = 0,
    ) -> Dict[str, Any]:
        data = await self._get_json(
            f"/library/metadata/{rating_key}",
            {"includeMarkers": "1", "includeChapters": "1"},
        )
        metas = data.get("MediaContainer", {}).get("Metadata", [])
        if not metas:
            raise KeyError(f"Plex item {rating_key} not found")
        it = metas[0]
        medias = it.get("Media") or []
        if not medias or not (medias[0].get("Part") or []):
            raise ValueError("Item has no playable media part")
        m0 = medias[0]
        p0 = m0["Part"][0]
        streams = p0.get("Stream") or []

        audio_streams = [s for s in streams if s.get("streamType") == 2]
        sub_streams = [s for s in streams if s.get("streamType") == 3]

        if audio_stream_id is not None:
            sel_audio = next((s for s in audio_streams if int(s.get("id") or 0) == audio_stream_id), None)
        else:
            sel_audio = next((s for s in audio_streams if s.get("selected")), audio_streams[0] if audio_streams else None)

        if sub_stream_id == 0:
            sel_sub = None
        elif sub_stream_id is not None:
            sel_sub = next((s for s in sub_streams if int(s.get("id") or 0) == sub_stream_id), None)
        else:
            sel_sub = next((s for s in sub_streams if s.get("selected")), None)

        decision = decide_playback(m0, p0, sel_audio, sel_sub, force_transcode=force_transcode)
        part_key = str(p0.get("key") or "")
        part_id = str(p0.get("id") or rating_key)

        if decision["mode"] == "direct_play":
            # Gateway range-proxied stream (keeps Plex token strictly server-side, 0% transcode CPU!)
            stream_url = f"{self.cfg.public_base_url}/v1/stream/plex/{rating_key}?part_key={quote(part_key, safe='')}"
        elif decision["mode"] == "direct_stream_audio":
            a_idx = int((sel_audio or {}).get("index") or 1)
            stream_url = (
                f"{self.cfg.public_base_url}/v1/stream/plex-remux/{rating_key}"
                f"?audio_index={a_idx}&acodec={decision['target_audio_codec']}"
                f"&channels={decision['audio_channels']}&start_ms={max(0, start_ms)}"
            )
        else:
            # Plex Intel QuickSync HLS transcode (proxied or local URL via gateway)
            stream_url = (
                f"{self.cfg.public_base_url}/v1/stream/plex-transcode/{rating_key}"
                f"?start_ms={max(0, start_ms)}"
                + (f"&sub_id={sel_sub['id']}" if sel_sub and decision["sub_mode"] == "burn_in" else "")
            )

        subtitle_url = ""
        if sel_sub and decision["sub_mode"] == "native_cues":
            subtitle_url = f"{self.cfg.public_base_url}/v1/subtitles/plex/{rating_key}/{sel_sub['id']}"

        markers = [
            {
                "type": str(m.get("type") or "").lower(),
                "start_ms": int(m.get("startTimeOffset") or 0),
                "end_ms": int(m.get("endTimeOffset") or 0),
                "action": "prompt_skip",
            }
            for m in (it.get("Marker") or [])
        ]
        chapters = [
            {
                "title": str(c.get("tag") or f"Chapter {c.get('index', '')}"),
                "start_ms": int(c.get("startTimeOffset") or 0),
                "end_ms": int(c.get("endTimeOffset") or 0),
            }
            for c in (it.get("Chapter") or [])
        ]
        up_next = await self._find_up_next(it)

        return {
            "id": f"plex:{rating_key}",
            "source": "plex",
            "rating_key": rating_key,
            "part_id": part_id,
            "title": self.normalize_card(it)["title"],
            "subtitle": self.normalize_card(it)["subtitle"],
            "duration_ms": int(it.get("duration") or m0.get("duration") or 0),
            "resume_ms": int(it.get("viewOffset") or 0),
            "stream_url": stream_url,
            "subtitle_url": subtitle_url,
            "decision": decision,
            "selected_audio_id": int(sel_audio["id"]) if sel_audio else 0,
            "selected_sub_id": int(sel_sub["id"]) if sel_sub else 0,
            "markers": markers,
            "chapters": chapters,
            "up_next": up_next,
        }

    async def get_local_media_file(self, rating_key: str) -> Optional[Path]:
        data = await self._get_json(f"/library/metadata/{rating_key}")
        metas = data.get("MediaContainer", {}).get("Metadata", [])
        if not metas:
            return None
        parts = (metas[0].get("Media") or [{}])[0].get("Part") or []
        if not parts:
            return None
        file_str = str(parts[0].get("file") or "")
        if not file_str:
            return None
        p = Path(file_str)
        if p.is_file():
            return p
        # Map Plex /data/... -> cfg.media_data_dir/...
        if file_str.startswith("/data/"):
            mapped = self.cfg.media_data_dir / file_str[len("/data/"):]
            if mapped.is_file():
                return mapped
        return None

    async def get_subtitle_cues(self, rating_key: str, stream_id: int) -> Dict[str, Any]:
        cache_key = hashlib.sha256(f"plex|{rating_key}|{stream_id}".encode()).hexdigest()
        cached = load_cached_cues(self.cfg.sub_cache_dir, cache_key)
        if cached is not None:
            return cached

        data = await self._get_json(f"/library/metadata/{rating_key}")
        metas = data.get("MediaContainer", {}).get("Metadata", [])
        if not metas:
            raise KeyError("Plex item not found")
        p0 = (metas[0].get("Media") or [{}])[0].get("Part", [{}])[0]
        streams = p0.get("Stream") or []
        sub = next((s for s in streams if s.get("streamType") == 3 and int(s.get("id") or 0) == stream_id), None)
        if not sub:
            raise KeyError(f"Subtitle stream {stream_id} not found")

        codec = str(sub.get("codec") or "srt").lower()
        lang = str(sub.get("languageCode") or sub.get("language") or "und")
        label = str(sub.get("displayTitle") or sub.get("language") or "Subtitle")

        raw_text = ""
        if sub.get("key"):
            # External sidecar or Plex-served stream
            url = f"{self.cfg.plex_url}{sub['key']}"
            async with self.session.get(url, headers=self._headers(), timeout=aiohttp.ClientTimeout(total=15)) as r:
                r.raise_for_status()
                raw_text = await r.text(errors="replace")
        else:
            # Embedded track in MKV/MP4 -> extract directly from local RO /data mount via ffmpeg!
            media_path = await self.get_local_media_file(rating_key)
            if not media_path:
                raise FileNotFoundError("Embedded subtitle extraction requires local /data media mount")
            stream_idx = int(sub.get("index") or 0)
            raw_text = await extract_embedded_subtitle_srt(media_path, stream_idx)
            codec = "srt"

        cues = parse_any_subtitle(raw_text, codec_hint=codec)
        payload = {
            "format": "cues_v1",
            "lang": lang,
            "label": label,
            "codec": codec,
            "count": len(cues),
            "cues": cues,
        }
        save_cached_cues(self.cfg.sub_cache_dir, cache_key, payload)
        return payload

    async def report_timeline(
        self,
        rating_key: str,
        state: str,
        time_ms: int,
        duration_ms: int,
    ) -> Dict[str, Any]:
        params = {
            "ratingKey": rating_key,
            "key": f"/library/metadata/{rating_key}",
            "state": state,  # playing | paused | stopped
            "time": str(max(0, int(time_ms))),
            "duration": str(max(0, int(duration_ms))),
        }
        async with self.session.get(
            f"{self.cfg.plex_url}/:/timeline",
            headers=self._headers(),
            params=params,
            timeout=aiohttp.ClientTimeout(total=5),
        ) as r:
            ok = r.status < 400
        scrobbled = False
        if duration_ms > 0 and time_ms >= int(duration_ms * 0.90) and state == "stopped":
            async with self.session.get(
                f"{self.cfg.plex_url}/:/scrobble",
                headers=self._headers(),
                params={"key": rating_key, "identifier": "com.plexapp.plugins.library"},
                timeout=aiohttp.ClientTimeout(total=5),
            ) as r2:
                scrobbled = r2.status < 400
        return {"ok": ok, "scrobbled": scrobbled}

    async def search(self, query: str, limit: int = 12) -> List[Dict[str, Any]]:
        if not query.strip():
            return []
        data = await self._get_json(
            "/hubs/search",
            {"query": query.strip(), "limit": str(max(limit, 15))},
        )
        primary: List[Dict[str, Any]] = []
        episodes: List[Dict[str, Any]] = []
        seen = set()
        matched_shows = set()
        for hub in data.get("MediaContainer", {}).get("Hub", []):
            htype = hub.get("type")
            if htype not in ("movie", "show", "episode"):
                continue
            for it in hub.get("Metadata", []):
                rk = str(it.get("ratingKey") or "")
                if not rk or rk in seen:
                    continue
                seen.add(rk)
                if htype in ("movie", "show"):
                    if htype == "show":
                        matched_shows.add(rk)
                    primary.append(self.normalize_card(it, card_art="poster"))
                else:
                    # Keep episode only if its parent show wasn't already returned as a primary show match
                    gp = str(it.get("grandparentRatingKey") or "")
                    if gp not in matched_shows:
                        episodes.append(self.normalize_card(it, card_art="thumb"))
        # Filter out episodes belonging to shows we saw later in the hub list
        episodes = [e for e in episodes if e.get("rating_key") not in matched_shows]
        combined = primary + episodes[: max(0, min(4, limit - len(primary)))]
        return combined[:limit]
