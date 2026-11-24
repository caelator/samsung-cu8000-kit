"""Unified YouTube InnerTube (TVHTML5 OAuth Device Login), SponsorBlock,
yt-dlp stream resolver, and zero-re-encode ffmpeg DASH remuxer.

All heavy work (OAuth refresh, InnerTube JSON parsing, yt-dlp extraction,
SponsorBlock lookup, DASH video+audio copy remuxing, and caption normalization)
runs on your local Docker host/NAS so the ARMv7 TV stays fast.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
import re
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import quote

import aiohttp

from .config import GatewayConfig
from .subtitles import load_cached_cues, parse_any_subtitle, save_cached_cues

INNERTUBE_TV_CONTEXT = {
    "client": {
        "clientName": "TVHTML5",
        "clientVersion": "7.20250923.13.00",
        "hl": "en",
        "gl": "US",
        "platform": "TV",
    }
}
INNERTUBE_WEB_CONTEXT = {
    "client": {
        "clientName": "WEB",
        "clientVersion": "2.20250923.00.00",
        "hl": "en",
        "gl": "US",
    }
}
YT_TV_UA = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version"
YT_WEB_UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36"
YT_OAUTH_SCOPE = "https://www.googleapis.com/auth/youtube"
SPONSORBLOCK_API = "https://sponsor.ajay.app/api/skipSegments"


def parse_duration_text_ms(text: str) -> int:
    """Parse '12:34' or '1:02:03' into milliseconds."""
    if not text:
        return 0
    parts = [p for p in re.findall(r"\d+", text)]
    if not parts:
        return 0
    nums = [int(p) for p in parts]
    if len(nums) == 3:
        sec = nums[0] * 3600 + nums[1] * 60 + nums[2]
    elif len(nums) == 2:
        sec = nums[0] * 60 + nums[1]
    else:
        sec = nums[0]
    return sec * 1000


def _extract_text(obj: Any) -> str:
    if isinstance(obj, str):
        return obj
    if not isinstance(obj, dict):
        return ""
    if "simpleText" in obj:
        return str(obj["simpleText"])
    if "content" in obj and isinstance(obj["content"], str):
        return obj["content"]
    runs = obj.get("runs")
    if isinstance(runs, list):
        return "".join(str(r.get("text", "")) for r in runs if isinstance(r, dict))
    return ""


def _best_thumb(thumbs: Any, video_id: str) -> str:
    if isinstance(thumbs, dict):
        thumbs = thumbs.get("thumbnails") or thumbs.get("sources") or []
    if isinstance(thumbs, list) and thumbs:
        best = max(
            (t for t in thumbs if isinstance(t, dict) and t.get("url")),
            key=lambda t: int(t.get("width") or 0),
            default=None,
        )
        if best:
            u = str(best["url"])
            if u.startswith("//"):
                u = "https:" + u
            return u
    return f"https://i.ytimg.com/vi/{video_id}/hqdefault.jpg"


def extract_videos_from_innertube(
    node: Any,
    out: List[Dict[str, Any]],
    seen: set,
    limit: int,
    skip_shorts: bool = True,
) -> None:
    """Recursively harvest video cards from any InnerTube TVHTML5/WEB JSON response."""
    if len(out) >= limit:
        return
    if isinstance(node, list):
        for item in node:
            extract_videos_from_innertube(item, out, seen, limit, skip_shorts=skip_shorts)
            if len(out) >= limit:
                return
        return
    if not isinstance(node, dict):
        return

    # Skip dedicated Shorts shelves when building living-room widescreen feeds
    if skip_shorts and ("reelShelfRenderer" in node or "shortsLockupViewModel" in node):
        return

    # Classic TVHTML5 tileRenderer
    if "tileRenderer" in node and isinstance(node["tileRenderer"], dict):
        tr = node["tileRenderer"]
        ctype = str(tr.get("contentType") or "")
        if skip_shorts and ("SHORT" in ctype.upper() or "reelWatchEndpoint" in tr.get("onSelectCommand", {})):
            return
        vid = (
            tr.get("onSelectCommand", {}).get("watchEndpoint", {}).get("videoId")
            or tr.get("contentId")
            or ""
        )
        if vid and len(vid) == 11 and vid not in seen:
            meta = tr.get("metadata", {}).get("tileMetadataRenderer", {})
            title = _extract_text(meta.get("title"))
            lines: List[str] = []
            for ln in meta.get("lines", []):
                if not isinstance(ln, dict):
                    continue
                lr = ln.get("lineRenderer")
                if isinstance(lr, dict):
                    parts = []
                    for it in lr.get("items", []):
                        if isinstance(it, dict) and "lineItemRenderer" in it:
                            txt = _extract_text(it["lineItemRenderer"].get("text")).strip()
                            if txt and txt not in ("•", "·"):
                                parts.append(txt)
                    if parts:
                        lines.append(" · ".join(parts))
                else:
                    txt = _extract_text(ln).strip()
                    if txt:
                        lines.append(txt)
            # TVHTML5 Shorts tiles omit `lines` completely; skip them so living-room feeds stay clean
            if skip_shorts and not lines:
                return
            hdr = tr.get("header", {}).get("tileHeaderRenderer", {})
            dur = ""
            for ov in hdr.get("thumbnailOverlays", []):
                if "thumbnailOverlayTimeStatusRenderer" in ov:
                    dur = _extract_text(ov["thumbnailOverlayTimeStatusRenderer"].get("text"))
            if skip_shorts and dur.strip().upper() == "SHORTS":
                return
            thumb = _best_thumb(hdr.get("thumbnail"), vid)
            if title:
                seen.add(vid)
                out.append(
                    {
                        "video_id": vid,
                        "title": title,
                        "channel": lines[0] if lines else "",
                        "meta": " · ".join(lines[1:3]) if len(lines) > 1 else "",
                        "duration_ms": parse_duration_text_ms(dur),
                        "duration_text": dur,
                        "thumb_raw": thumb,
                    }
                )
        return

    # Standard WEB/TV renderers
    for rkey in ("videoRenderer", "gridVideoRenderer", "compactVideoRenderer", "playlistVideoRenderer"):
        if rkey in node and isinstance(node[rkey], dict):
            vr = node[rkey]
            vid = str(vr.get("videoId") or "")
            if vid and len(vid) == 11 and vid not in seen:
                title = _extract_text(vr.get("title"))
                channel = _extract_text(vr.get("ownerText") or vr.get("shortBylineText") or vr.get("longBylineText"))
                views = _extract_text(vr.get("shortViewCountText") or vr.get("viewCountText"))
                pub = _extract_text(vr.get("publishedTimeText"))
                dur = _extract_text(vr.get("lengthText"))
                if skip_shorts and dur.strip().upper() == "SHORTS":
                    return
                thumb = _best_thumb(vr.get("thumbnail"), vid)
                if title:
                    seen.add(vid)
                    meta_parts = [p for p in (views, pub) if p]
                    out.append(
                        {
                            "video_id": vid,
                            "title": title,
                            "channel": channel,
                            "meta": " · ".join(meta_parts),
                            "duration_ms": parse_duration_text_ms(dur),
                            "duration_text": dur,
                            "thumb_raw": thumb,
                        }
                    )
            return

    # Modern lockupViewModel
    if "lockupViewModel" in node and isinstance(node["lockupViewModel"], dict):
        lv = node["lockupViewModel"]
        vid = str(lv.get("contentId") or "")
        if vid and len(vid) == 11 and vid not in seen:
            md = lv.get("metadata", {}).get("lockupMetadataViewModel", {})
            title = _extract_text(md.get("title"))
            rows = md.get("metadata", {}).get("contentMetadataViewModel", {}).get("metadataRows", [])
            parts = []
            for row in rows:
                for p in row.get("metadataParts", []):
                    t = _extract_text(p.get("text"))
                    if t:
                        parts.append(t)
            img = lv.get("contentImage", {}).get("thumbnailViewModel", {}).get("image")
            thumb = _best_thumb(img, vid)
            if title:
                seen.add(vid)
                out.append(
                    {
                        "video_id": vid,
                        "title": title,
                        "channel": parts[0] if parts else "",
                        "meta": " · ".join(parts[1:3]) if len(parts) > 1 else "",
                        "duration_ms": 0,
                        "duration_text": "",
                        "thumb_raw": thumb,
                    }
                )
        return

    for v in node.values():
        extract_videos_from_innertube(v, out, seen, limit, skip_shorts=skip_shorts)
        if len(out) >= limit:
            return


# Curated Kids channels (seeded from verified ytdl-sub & family subscriptions; fully overridable in youtube_channels.json)
CURATED_KIDS_CHANNELS: List[Dict[str, str]] = [
    {
        "channel_id": "UC5PYHgAzJ1wLEidB58SK6Xw",
        "title": "Blippi - Educational Videos for Kids",
        "avatar": "https://yt3.googleusercontent.com/ytc/AIdro_n8W4eH0v4fM8n3mH9l_o4mM6qV9rS4eJ8pM8k=s240-c-k-c0x00ffffff-no-rj",
    },
    {
        "channel_id": "UC_dpB5SImaseMZpLu8s-r7w",
        "title": "Blippi Wonders - Educational Cartoons for Kids",
        "avatar": "",
    },
    {
        "channel_id": "UCjL3lA1cygbQuSrarxWxQ4A",
        "title": "Blippi's Vehicle Adventures",
        "avatar": "",
    },
    {
        "channel_id": "UCG2CL6EUjG8TVT1Tpl9nJdg",
        "title": "Ms Rachel - Toddler Learning Videos",
        "avatar": "",
    },
    {
        "channel_id": "UCRv76wLBC73jiP7LX4C3l8Q",
        "title": "Маша и Медведь",
        "avatar": "",
    },
    {
        "channel_id": "UCEtlXZzzllBh9x-kEiENMLg",
        "title": "Tuttle Twins",
        "avatar": "",
    },
]

CURATED_KIDS_IDS = frozenset(c["channel_id"] for c in CURATED_KIDS_CHANNELS)

KIDS_NAME_KEYWORDS = (
    "blippi",
    "meekah",
    "ms rachel",
    "toddler",
    "cocomelon",
    "super simple",
    "pinkfong",
    "bluey",
    "peppa pig",
    "paw patrol",
    "sesame street",
    "маша и медведь",
    "masha and the bear",
    "tuttle twins",
    "numberblocks",
    "alphablocks",
    "daniel tiger",
    "hey bear",
    "kids songs",
    "nursery rhymes",
    "for kids",
)

ADULT_CATEGORIES: List[Tuple[str, str]] = [
    ("active", "Active"),
    ("favorites", "Favorites"),
    ("woodworking", "Woodworking & Making"),
    ("home", "Home & Design"),
    ("tech", "Tech & AI"),
    ("cars", "Cars & Gear"),
    ("podcasts", "Podcasts & Talk"),
    ("lifestyle", "Health & More"),
    ("all", "All A–Z"),
]

CATEGORY_LABELS: Dict[str, str] = dict(ADULT_CATEGORIES)

_CAT_RULES: List[Tuple[str, Tuple[str, ...]]] = [
    (
        "woodworking",
        (
            "wood", "maker", "make", "craft", "furniture", "carpenter", "lumber", "machin",
            "tested", "mechanics", "restoration", "watchmaker", "wristwatch", "shop nation",
            "tool", "fab", "leather", "pask", "sellers", "cremona", "estlea", "cosman",
            "katz-moses", "blacktail", "foureyes", "bourbon moth", "3x3custom", "731",
            "colinfurze", "mark rober", "stuff made here", "zack freedman", "izzy swan",
            "malecki", "pedulla", "woodomain", "festool", "paulk", "dust", "chaffin",
            "built knot", "jonny builds", "coffey", "hacksmith", "hand tool", "morley",
            "waters & acland", "spencley", "sawyer design", "shaun boyd", "chappel",
            "rawls", "svanfon", "reborn tools", "works by design", "zen-wu",
        ),
    ),
    (
        "home",
        (
            "house", "home", "architectural", "chateau", "ruin", "renovation", "rénovation",
            "renovision", "france", "cabin", "haus", "houzz", "risinger", "build show",
            "perkins", "catalana", "farm", "norwegian", "van", "tiny shiny", "builders",
            "bone structure", "revent", "indie projects", "flippingdrawers",
        ),
    ),
    (
        "tech",
        (
            " ai", "ai ", "evolving ai", "tech", "dev", "engineer", "hardware", "linus",
            "networkchuck", "ziskind", "firecrawl", "billet labs", "bps.space", "xkcd",
            "scitapas", "visual thinker", "basecamp", "rctestflight", "turbulent",
            "james bruton", "labcoatz", "find404", "alpha central",
        ),
    ),
    (
        "cars",
        (
            "auto", "car", "truck", "garage", "drive", "donut", "hagerty", "rod", "airsoft",
            "airgun", "hunting", "armory", "paragon", "nocpix", "corbin", "sharks", "sub12",
            "wyshtech", "anttech", "caseys customs", "outlaw", "classic nation", "muscle",
        ),
    ),
    (
        "podcasts",
        (
            "podcast", "pod", "diary of a ceo", "flagrant", "babylon bee", "ryan long",
            "bachelor", "manosphere", "rational", "pearl", "dating", "whatever", "dad saves",
            "psychology", "alex clark", "erb", "omeleto", "finance", "alexander grace",
            "man guide", "modern women", "street talks", "wingmam", "peasant party",
            "masked auditor", "mandatoryfunday", "hoe_math", "caitlin v", "karen straughan",
            "blaine anderson", "courtney ryan", "emilywking", "melanie king", "sadia",
            "taylor the fiend", "thadd boii", "destro", "dustups",
        ),
    ),
]


def classify_channel_audience(channel_id: str, title: str, prefs: Dict[str, Any]) -> str:
    kids_set = set(prefs.get("kids_ids") or [])
    adults_set = set(prefs.get("adults_ids") or [])
    if channel_id in kids_set:
        return "kids"
    if channel_id in adults_set:
        return "adults"
    if channel_id in CURATED_KIDS_IDS:
        return "kids"
    low = (title or "").lower()
    if any(kw in low for kw in KIDS_NAME_KEYWORDS):
        return "kids"
    return "adults"


def classify_adult_category(channel_id: str, title: str, prefs: Dict[str, Any]) -> str:
    overrides = prefs.get("category_overrides") or {}
    if channel_id in overrides and overrides[channel_id] in CATEGORY_LABELS:
        return str(overrides[channel_id])
    low = f" {(title or '').lower()} "
    for cat, kws in _CAT_RULES:
        if any(kw in low for kw in kws):
            return cat
    return "lifestyle"


def extract_channels_from_innertube(node: Any, out: List[Dict[str, str]], seen: set) -> None:
    """Extract subscribed YouTube channels (UC... ID, title, avatar URL) from InnerTube FEchannels."""
    if isinstance(node, list):
        for x in node:
            extract_channels_from_innertube(x, out, seen)
        return
    if not isinstance(node, dict):
        return
    for k in ("channelRenderer", "gridChannelRenderer", "tileRenderer"):
        if k in node and isinstance(node[k], dict):
            r = node[k]
            cid = (
                r.get("channelId")
                or r.get("navigationEndpoint", {}).get("browseEndpoint", {}).get("browseId")
                or r.get("onSelectCommand", {}).get("browseEndpoint", {}).get("browseId")
                or ""
            )
            if cid.startswith("UC") and cid not in seen:
                title = _extract_text(
                    r.get("title") or r.get("metadata", {}).get("tileMetadataRenderer", {}).get("title")
                ).strip()
                th = r.get("thumbnail") or r.get("header", {}).get("tileHeaderRenderer", {}).get("thumbnail") or {}
                thumbs = th.get("thumbnails") or []
                av = str(thumbs[-1].get("url") or "") if thumbs else ""
                if av.startswith("//"):
                    av = "https:" + av
                if title:
                    seen.add(cid)
                    out.append({"channel_id": cid, "title": title, "avatar": av})
            return
    for v in node.values():
        extract_channels_from_innertube(v, out, seen)


class YouTubeClient:
    def __init__(self, cfg: GatewayConfig, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.session = session
        self._oauth: Dict[str, Any] = self._load_oauth()
        self._pending_device: Dict[str, Any] = {}
        self._tv_creds_cache: Tuple[float, str, str] = (0.0, "", "")
        self._stream_cache: Dict[str, Tuple[float, Dict[str, Any]]] = {}
        self._sb_cache: Dict[str, Tuple[float, List[Dict[str, Any]]]] = {}
        self._state: Dict[str, Any] = self._load_state()
        self._channel_prefs: Dict[str, Any] = self._load_channel_prefs()
        self._channels_cache: Tuple[float, List[Dict[str, Any]]] = (0.0, [])
        self._channel_vids_cache: Dict[str, Tuple[float, Dict[str, Any]]] = {}
        self._recent_active_names: Tuple[float, Dict[str, int]] = (0.0, {})
        self._resolved_meta_cache: Dict[str, Dict[str, str]] = {}
        self._kids_feed_cache: Tuple[float, List[Dict[str, Any]]] = (0.0, [])
        self._ytdl_sub_index: Tuple[float, Dict[str, str]] = (0.0, {})

    # ── Persistent local state & OAuth token storage (mode 0600, never logged) ──

    def _load_oauth(self) -> Dict[str, Any]:
        p = self.cfg.youtube_oauth_path
        if not p.is_file():
            return {}
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            return {}

    def _save_oauth(self, data: Dict[str, Any]) -> None:
        self._oauth = data
        p = self.cfg.youtube_oauth_path
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(f".tmp.{os.getpid()}")
        tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
        os.chmod(tmp, 0o600)
        os.replace(tmp, p)

    def _load_state(self) -> Dict[str, Any]:
        p = self.cfg.state_path
        if not p.is_file():
            return {"youtube_progress": {}}
        try:
            st = json.loads(p.read_text(encoding="utf-8"))
            st.setdefault("youtube_progress", {})
            return st
        except Exception:
            return {"youtube_progress": {}}

    def _save_state(self) -> None:
        p = self.cfg.state_path
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(f".tmp.{os.getpid()}")
        tmp.write_text(json.dumps(self._state, separators=(",", ":")), encoding="utf-8")
        os.replace(tmp, p)

    def _load_channel_prefs(self) -> Dict[str, Any]:
        default = {
            "kids_ids": [],
            "adults_ids": [],
            "favorite_ids": [],
            "category_overrides": {},
            "custom_channels": [],
        }
        p = self.cfg.youtube_channels_path
        if not p.is_file():
            return default
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
            for k, v in default.items():
                data.setdefault(k, v)
            return data
        except Exception:
            return default

    def _save_channel_prefs(self) -> None:
        p = self.cfg.youtube_channels_path
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(f".tmp.{os.getpid()}")
        tmp.write_text(json.dumps(self._channel_prefs, indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, p)

    def _invalidate_channel_caches(self) -> None:
        self._channels_cache = (0.0, [])
        self._kids_feed_cache = (0.0, [])

    def _art_url(self, preset: str, url: str) -> str:
        if not url:
            return ""
        return f"{self.cfg.public_base_url}/v1/art/{preset}?src={quote(url, safe='')}"

    def is_signed_in(self) -> bool:
        return bool(self._oauth.get("refresh_token"))

    def health(self) -> Dict[str, Any]:
        return {
            "signed_in": self.is_signed_in(),
            "sponsorblock_enabled": self.cfg.sponsorblock_enabled,
            "auth_pending": bool(self._pending_device.get("user_code")),
            "progress_items": len(self._state.get("youtube_progress", {})),
        }

    # ── OAuth 2.0 Device Flow (google.com/device) ──

    async def _get_tv_oauth_client(self) -> Tuple[str, str]:
        """Load optional user client credentials or fetch public TVHTML5 OAuth client."""
        custom_path = self.cfg.config_dir / "youtube_oauth_client.json"
        if custom_path.is_file():
            try:
                c = json.loads(custom_path.read_text(encoding="utf-8"))
                if c.get("client_id") and c.get("client_secret"):
                    return str(c["client_id"]), str(c["client_secret"])
            except Exception:
                pass

        if self._oauth.get("client_id") and self._oauth.get("client_secret"):
            return str(self._oauth["client_id"]), str(self._oauth["client_secret"])

        now = time.monotonic()
        if self._tv_creds_cache[1] and (now - self._tv_creds_cache[0]) < 3600:
            return self._tv_creds_cache[1], self._tv_creds_cache[2]

        # Dynamically extract current TVHTML5 public device client from youtube.com/tv
        async with self.session.get(
            "https://www.youtube.com/tv",
            headers={"User-Agent": YT_TV_UA},
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            html_body = await r.text(errors="replace")

        m_cred = re.search(
            r'clientId\s*:\s*["\']([0-9]{8,}-[A-Za-z0-9_-]+\.apps\.googleusercontent\.com)["\']\s*,\s*'
            r'[A-Za-z0-9_$]{1,16}\s*:\s*["\']([A-Za-z0-9_-]{18,64})["\']',
            html_body,
        )
        if not m_cred:
            m_kab = re.search(r'(/s/_/kabuki/_/js/[^"\'\s<>]+)', html_body)
            if m_kab:
                js_url = "https://www.youtube.com" + m_kab.group(1)
                async with self.session.get(
                    js_url,
                    headers={"User-Agent": YT_TV_UA},
                    timeout=aiohttp.ClientTimeout(total=15),
                ) as r2:
                    js_body = await r2.text(errors="replace")
                m_cred = re.search(
                    r'clientId\s*:\s*["\']([0-9]{8,}-[A-Za-z0-9_-]+\.apps\.googleusercontent\.com)["\']\s*,\s*'
                    r'[A-Za-z0-9_$]{1,16}\s*:\s*["\']([A-Za-z0-9_-]{18,64})["\']\s*,\s*scopes',
                    js_body,
                )
        if not m_cred:
            raise RuntimeError(
                "YouTube TV OAuth client extraction did not match; provide /config/youtube_oauth_client.json"
            )
        cid, csec = m_cred.group(1), m_cred.group(2)
        self._tv_creds_cache = (now, cid, csec)
        return cid, csec

    async def start_device_auth(self) -> Dict[str, Any]:
        cid, csec = await self._get_tv_oauth_client()
        async with self.session.post(
            "https://oauth2.googleapis.com/device/code",
            data={"client_id": cid, "scope": YT_OAUTH_SCOPE},
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            data = await r.json(content_type=None)
            if r.status >= 400:
                raise RuntimeError(f"Google device auth failed: {data.get('error_description') or data}")
        now = time.time()
        self._pending_device = {
            "device_code": data["device_code"],
            "user_code": data["user_code"],
            "verification_url": data.get("verification_url") or "https://www.google.com/device",
            "interval": int(data.get("interval") or 5),
            "expires_at": now + int(data.get("expires_in") or 1800),
            "client_id": cid,
            "client_secret": csec,
        }
        return {
            "verification_url": self._pending_device["verification_url"],
            "user_code": self._pending_device["user_code"],
            "expires_in": int(data.get("expires_in") or 1800),
            "interval": self._pending_device["interval"],
        }

    async def poll_device_auth(self) -> Dict[str, Any]:
        if self.is_signed_in():
            return {"status": "signed_in", "signed_in": True}
        pending = self._pending_device
        if not pending:
            return {"status": "signed_out", "signed_in": False}
        if time.time() >= pending["expires_at"]:
            self._pending_device = {}
            return {"status": "expired", "signed_in": False}

        async with self.session.post(
            "https://oauth2.googleapis.com/token",
            data={
                "client_id": pending["client_id"],
                "client_secret": pending["client_secret"],
                "device_code": pending["device_code"],
                "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            },
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            data = await r.json(content_type=None)

        if "access_token" in data and "refresh_token" in data:
            self._save_oauth(
                {
                    "access_token": data["access_token"],
                    "refresh_token": data["refresh_token"],
                    "expires_at": time.time() + int(data.get("expires_in") or 3600),
                    "client_id": pending["client_id"],
                    "client_secret": pending["client_secret"],
                }
            )
            self._pending_device = {}
            return {"status": "signed_in", "signed_in": True}

        err = data.get("error", "authorization_pending")
        if err in ("authorization_pending", "slow_down"):
            return {
                "status": "pending",
                "signed_in": False,
                "verification_url": pending["verification_url"],
                "user_code": pending["user_code"],
                "remaining_s": max(0, int(pending["expires_at"] - time.time())),
            }
        self._pending_device = {}
        return {"status": "error", "signed_in": False, "error": err}

    def logout(self) -> Dict[str, Any]:
        self._oauth = {}
        self._pending_device = {}
        if self.cfg.youtube_oauth_path.exists():
            self.cfg.youtube_oauth_path.unlink()
        return {"ok": True, "signed_in": False}

    async def _get_access_token(self) -> str:
        if not self.is_signed_in():
            return ""
        if time.time() < float(self._oauth.get("expires_at") or 0) - 120:
            return str(self._oauth.get("access_token") or "")
        async with self.session.post(
            "https://oauth2.googleapis.com/token",
            data={
                "client_id": self._oauth["client_id"],
                "client_secret": self._oauth["client_secret"],
                "refresh_token": self._oauth["refresh_token"],
                "grant_type": "refresh_token",
            },
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            data = await r.json(content_type=None)
            if r.status >= 400 or "access_token" not in data:
                return ""
        updated = dict(self._oauth)
        updated["access_token"] = data["access_token"]
        updated["expires_at"] = time.time() + int(data.get("expires_in") or 3600)
        self._save_oauth(updated)
        return str(updated["access_token"])

    # ── Local ytdl-sub archive index (instant local disk playback when already saved!) ──

    def _scan_ytdl_sub_index(self) -> Dict[str, str]:
        now = time.monotonic()
        if self._ytdl_sub_index[1] and (now - self._ytdl_sub_index[0]) < 300:
            return self._ytdl_sub_index[1]
        idx: Dict[str, str] = {}
        kids_tv = self.cfg.media_data_dir / "Library" / "kids" / "tv"
        if kids_tv.is_dir():
            vid_re = re.compile(r"\[([A-Za-z0-9_-]{11})\]")
            for root, _, files in os.walk(kids_tv):
                for fn in files:
                    if not fn.lower().endswith((".mp4", ".mkv", ".webm")):
                        continue
                    m = vid_re.search(fn)
                    if m:
                        idx[m.group(1)] = os.path.join(root, fn)
        self._ytdl_sub_index = (now, idx)
        return idx

    # ── InnerTube Browse & Search ──

    async def _innertube_post(self, endpoint: str, body: Dict[str, Any], use_tv: bool = True) -> Dict[str, Any]:
        token = await self._get_access_token() if use_tv else ""
        ctx = INNERTUBE_TV_CONTEXT if (use_tv and token) else INNERTUBE_WEB_CONTEXT
        payload = {"context": ctx, **body}
        headers = {
            "Content-Type": "application/json",
            "User-Agent": YT_TV_UA if (use_tv and token) else YT_WEB_UA,
            "Origin": "https://www.youtube.com",
        }
        if token and use_tv:
            headers["Authorization"] = f"Bearer {token}"
        async with self.session.post(
            f"https://www.youtube.com/youtubei/v1/{endpoint}?prettyPrint=false",
            headers=headers,
            json=payload,
            timeout=aiohttp.ClientTimeout(total=12),
        ) as r:
            r.raise_for_status()
            return await r.json(content_type=None)

    def _to_unified_card(self, raw: Dict[str, Any]) -> Dict[str, Any]:
        vid = raw["video_id"]
        prog_entry = self._state.get("youtube_progress", {}).get(vid, {})
        dur_ms = int(raw.get("duration_ms") or prog_entry.get("duration_ms") or 0)
        offset_ms = int(prog_entry.get("offset_ms") or 0)
        progress = round(min(1.0, offset_ms / dur_ms), 3) if dur_ms > 0 and offset_ms > 0 else 0.0
        local_map = self._scan_ytdl_sub_index()
        local_archived = vid in local_map
        thumb = raw.get("thumb_raw") or f"https://i.ytimg.com/vi/{vid}/hqdefault.jpg"
        sub_parts = [p for p in (raw.get("channel"), raw.get("duration_text"), raw.get("meta")) if p]
        return {
            "id": f"yt:{vid}",
            "source": "youtube",
            "video_id": vid,
            "type": "video",
            "is_kids": False,
            "title": raw.get("title") or vid,
            "subtitle": " · ".join(sub_parts[:2]),
            "channel": raw.get("channel") or "",
            "duration_ms": dur_ms,
            "offset_ms": offset_ms,
            "progress": progress,
            "local_archived": local_archived,
            "thumb_astc": self._art_url("thumb", thumb),
            "art_astc": self._art_url("thumb", thumb),
            "backdrop_astc": self._art_url("backdrop", thumb),
        }

    async def _fetch_channel_header(self, channel_id: str) -> Dict[str, str]:
        cached = self._resolved_meta_cache.get(channel_id)
        if cached:
            return cached
        try:
            data = await self._innertube_post("browse", {"browseId": channel_id}, use_tv=False)
            md = data.get("metadata", {}).get("channelMetadataRenderer", {})
            title = str(md.get("title") or "").strip()
            thumbs = md.get("avatar", {}).get("thumbnails") or []
            av = str(thumbs[-1].get("url") or "") if thumbs else ""
            if av.startswith("//"):
                av = "https:" + av
            res = {"channel_id": channel_id, "title": title, "avatar": av}
            self._resolved_meta_cache[channel_id] = res
            return res
        except Exception:
            return {"channel_id": channel_id, "title": "", "avatar": ""}

    async def _get_recent_active_channels(self) -> Dict[str, int]:
        now = time.monotonic()
        if self._recent_active_names[1] and (now - self._recent_active_names[0]) < 300:
            return self._recent_active_names[1]
        counts: Dict[str, int] = {}
        if self.is_signed_in():
            try:
                data = await self._innertube_post("browse", {"browseId": "FEsubscriptions"}, use_tv=True)
                vids: List[Dict[str, Any]] = []
                extract_videos_from_innertube(data, vids, set(), 60, skip_shorts=True)
                for idx, v in enumerate(vids):
                    ch = (v.get("channel") or "").strip().lower()
                    if ch and ch not in counts:
                        # Higher score = more recently uploaded
                        counts[ch] = 1000 - idx
            except Exception:
                pass
        self._recent_active_names = (now, counts)
        return counts

    async def _get_all_channels_raw(self) -> List[Dict[str, Any]]:
        now = time.monotonic()
        if self._channels_cache[1] and (now - self._channels_cache[0]) < 900:
            return self._channels_cache[1]

        by_id: Dict[str, Dict[str, str]] = {}
        # 1. All signed-in YouTube Subscriptions via InnerTube FEchannels
        if self.is_signed_in():
            try:
                data = await self._innertube_post("browse", {"browseId": "FEchannels"}, use_tv=True)
                subs: List[Dict[str, str]] = []
                extract_channels_from_innertube(data, subs, set())
                for c in subs:
                    by_id[c["channel_id"]] = dict(c)
            except Exception:
                pass

        # 2. Merge curated Kids channels (ytdl-sub + family favorites) and user custom channels
        extra_missing_av: List[str] = []
        for src_list in (CURATED_KIDS_CHANNELS, self._channel_prefs.get("custom_channels") or []):
            for c in src_list:
                cid = str(c.get("channel_id") or "")
                if not cid.startswith("UC"):
                    continue
                if cid not in by_id:
                    by_id[cid] = {
                        "channel_id": cid,
                        "title": str(c.get("title") or cid),
                        "avatar": str(c.get("avatar") or ""),
                    }
                    if not by_id[cid]["avatar"]:
                        extra_missing_av.append(cid)
                elif not by_id[cid].get("avatar") and c.get("avatar"):
                    by_id[cid]["avatar"] = str(c["avatar"])

        if extra_missing_av:
            metas = await asyncio.gather(*(self._fetch_channel_header(cid) for cid in extra_missing_av[:12]))
            for m in metas:
                cid = m["channel_id"]
                if cid in by_id:
                    if m.get("title"):
                        by_id[cid]["title"] = m["title"]
                    if m.get("avatar"):
                        by_id[cid]["avatar"] = m["avatar"]

        active_scores = await self._get_recent_active_channels()
        favs = set(self._channel_prefs.get("favorite_ids") or [])
        enriched: List[Dict[str, Any]] = []
        for cid, raw in by_id.items():
            title = raw.get("title") or cid
            aud = classify_channel_audience(cid, title, self._channel_prefs)
            cat = "kids" if aud == "kids" else classify_adult_category(cid, title, self._channel_prefs)
            is_fav = cid in favs
            act_score = active_scores.get(title.strip().lower(), 0)
            av_url = raw.get("avatar") or ""
            art = self._art_url("channel", av_url) if av_url else ""
            sub_lbl = (
                "Kids YouTube Channel"
                if aud == "kids"
                else (
                    f"{CATEGORY_LABELS.get(cat, 'YouTube')} · Active"
                    if act_score > 0
                    else CATEGORY_LABELS.get(cat, "YouTube")
                )
            )
            if is_fav:
                sub_lbl = f"★ Favorite · {sub_lbl}"
            enriched.append(
                {
                    "id": cid,
                    "channel_id": cid,
                    "source": "yt_channel",
                    "type": "channel",
                    "audience": aud,
                    "is_kids": aud == "kids",
                    "category": cat,
                    "category_label": "Kids" if aud == "kids" else CATEGORY_LABELS.get(cat, "Other"),
                    "is_favorite": is_fav,
                    "active_score": act_score,
                    "title": title,
                    "subtitle": sub_lbl,
                    "avatar_raw": av_url,
                    "art_astc": art,
                    "thumb_astc": art,
                    "poster_astc": art,
                }
            )

        self._channels_cache = (now, enriched)
        return enriched

    async def get_channels(
        self,
        audience: str = "adults",
        category: str = "all",
        query: str = "",
    ) -> Dict[str, Any]:
        all_ch = await self._get_all_channels_raw()
        aud = (audience or "adults").lower()
        cat = (category or "all").lower()
        q = (query or "").strip().lower()

        scoped = [c for c in all_ch if aud == "all" or c["audience"] == aud]

        # Build category counts for the selected audience
        cat_counts: Dict[str, int] = {}
        for c in scoped:
            cat_counts[c["category"]] = cat_counts.get(c["category"], 0) + 1
            if c["active_score"] > 0:
                cat_counts["active"] = cat_counts.get("active", 0) + 1
            if c["is_favorite"]:
                cat_counts["favorites"] = cat_counts.get("favorites", 0) + 1

        if aud == "kids":
            filtered = list(scoped)
            filtered.sort(key=lambda c: (0 if c["is_favorite"] else 1, -c["active_score"], c["title"].lower()))
            cats_out = [{"id": "all", "label": "Kids Channels", "count": len(filtered)}]
        else:
            if cat == "active":
                filtered = [c for c in scoped if c["active_score"] > 0 or c["is_favorite"]]
                if not filtered:
                    filtered = list(scoped)
                filtered.sort(key=lambda c: (0 if c["is_favorite"] else 1, -c["active_score"], c["title"].lower()))
            elif cat == "favorites":
                filtered = [c for c in scoped if c["is_favorite"]]
                filtered.sort(key=lambda c: (-c["active_score"], c["title"].lower()))
            elif cat != "all":
                filtered = [c for c in scoped if c["category"] == cat]
                filtered.sort(key=lambda c: (0 if c["is_favorite"] else 1, -c["active_score"], c["title"].lower()))
            else:
                filtered = list(scoped)
                filtered.sort(key=lambda c: (0 if c["is_favorite"] else 1, c["title"].lower()))
            cats_out = [
                {
                    "id": cid,
                    "label": lbl,
                    "count": len(scoped) if cid == "all" else cat_counts.get(cid, 0),
                }
                for cid, lbl in ADULT_CATEGORIES
            ]

        if q:
            filtered = [c for c in filtered if q in c["title"].lower() or q in c["category_label"].lower()]

        return {
            "audience": aud,
            "category": cat,
            "signed_in": self.is_signed_in(),
            "total": len(filtered),
            "kids_count": sum(1 for c in all_ch if c["audience"] == "kids"),
            "adults_count": sum(1 for c in all_ch if c["audience"] == "adults"),
            "categories": cats_out,
            "items": filtered,
        }

    async def get_channel_videos(self, channel_id: str, limit: int = 32) -> Dict[str, Any]:
        cid = (channel_id or "").strip()
        if not cid.startswith("UC"):
            raise ValueError("Invalid YouTube channel_id")
        now = time.monotonic()
        cached = self._channel_vids_cache.get(cid)
        if cached and (now - cached[0]) < 300:
            # Refresh audience/favorite metadata dynamically even when video list is cached
            all_ch = await self._get_all_channels_raw()
            match = next((c for c in all_ch if c["channel_id"] == cid), None)
            out = dict(cached[1])
            if match:
                out["audience"] = match["audience"]
                out["is_kids"] = match["is_kids"]
                out["is_favorite"] = match["is_favorite"]
                out["category"] = match["category"]
                out["category_label"] = match["category_label"]
            return out

        # params="EgZ2aWRlb3PyBgQKAjoA" requests the channel's Videos tab (full-length videos, zero Shorts clutter!)
        data = await self._innertube_post(
            "browse",
            {"browseId": cid, "params": "EgZ2aWRlb3PyBgQKAjoA"},
            use_tv=False,
        )
        md = data.get("metadata", {}).get("channelMetadataRenderer", {})
        title = str(md.get("title") or "").strip()
        thumbs = md.get("avatar", {}).get("thumbnails") or []
        av = str(thumbs[-1].get("url") or "") if thumbs else ""
        if av.startswith("//"):
            av = "https:" + av

        raw_vids: List[Dict[str, Any]] = []
        extract_videos_from_innertube(data, raw_vids, set(), limit, skip_shorts=True)
        for v in raw_vids:
            if not v.get("channel") and title:
                v["channel"] = title

        aud = classify_channel_audience(cid, title, self._channel_prefs)
        cat = "kids" if aud == "kids" else classify_adult_category(cid, title, self._channel_prefs)
        is_fav = cid in set(self._channel_prefs.get("favorite_ids") or [])
        cards = [self._to_unified_card(v) for v in raw_vids]
        for card in cards:
            card["is_kids"] = (aud == "kids")
            card["channel_id"] = cid
        payload = {
            "channel_id": cid,
            "title": title or cid,
            "audience": aud,
            "is_kids": aud == "kids",
            "category": cat,
            "category_label": "Kids" if aud == "kids" else CATEGORY_LABELS.get(cat, "YouTube"),
            "is_favorite": is_fav,
            "avatar_astc": self._art_url("channel", av) if av else "",
            "total": len(cards),
            "items": cards,
        }
        self._channel_vids_cache[cid] = (now, payload)
        return payload

    def update_channel_prefs(
        self,
        channel_id: str,
        audience: Optional[str] = None,
        favorite: Optional[bool] = None,
        category: Optional[str] = None,
    ) -> Dict[str, Any]:
        cid = (channel_id or "").strip()
        if not cid.startswith("UC"):
            raise ValueError("Invalid YouTube channel_id")
        kids_ids = list(self._channel_prefs.get("kids_ids") or [])
        adults_ids = list(self._channel_prefs.get("adults_ids") or [])
        fav_ids = list(self._channel_prefs.get("favorite_ids") or [])
        cat_ov = dict(self._channel_prefs.get("category_overrides") or {})

        if audience in ("kids", "adults"):
            kids_ids = [x for x in kids_ids if x != cid]
            adults_ids = [x for x in adults_ids if x != cid]
            if audience == "kids":
                kids_ids.append(cid)
            else:
                adults_ids.append(cid)

        if favorite is not None:
            fav_ids = [x for x in fav_ids if x != cid]
            if favorite:
                fav_ids.append(cid)

        if category and category in CATEGORY_LABELS and category not in ("all", "active", "favorites"):
            cat_ov[cid] = category

        self._channel_prefs["kids_ids"] = kids_ids
        self._channel_prefs["adults_ids"] = adults_ids
        self._channel_prefs["favorite_ids"] = fav_ids
        self._channel_prefs["category_overrides"] = cat_ov
        self._save_channel_prefs()
        self._invalidate_channel_caches()
        return {
            "ok": True,
            "channel_id": cid,
            "audience": "kids" if cid in kids_ids else ("adults" if cid in adults_ids else audience),
            "is_favorite": cid in fav_ids,
            "category": cat_ov.get(cid, ""),
        }

    async def add_custom_channel(self, url_or_handle: str, audience: str = "kids") -> Dict[str, Any]:
        raw = (url_or_handle or "").strip()
        if not raw:
            raise ValueError("Channel handle, URL, or UC ID required")
        cid = ""
        if raw.startswith("UC") and len(raw) >= 20:
            cid = raw
        else:
            if raw.startswith("@"):
                url = f"https://www.youtube.com/{raw}"
            elif raw.startswith("http"):
                url = raw
            else:
                url = f"https://www.youtube.com/@{raw}"
            res = await self._innertube_post("navigation/resolve_url", {"url": url}, use_tv=False)
            cid = str(res.get("endpoint", {}).get("browseEndpoint", {}).get("browseId") or "")
        if not cid.startswith("UC"):
            raise ValueError(f"Could not resolve YouTube channel from '{raw}'")

        meta = await self._fetch_channel_header(cid)
        title = meta.get("title") or cid
        av = meta.get("avatar") or ""
        customs = [c for c in (self._channel_prefs.get("custom_channels") or []) if c.get("channel_id") != cid]
        customs.append({"channel_id": cid, "title": title, "avatar": av})
        self._channel_prefs["custom_channels"] = customs
        self.update_channel_prefs(cid, audience=audience if audience in ("kids", "adults") else "kids")
        return {"ok": True, "channel_id": cid, "title": title, "audience": audience}

    async def _is_kids_video(self, raw_vid: Dict[str, Any]) -> bool:
        ch_name = (raw_vid.get("channel") or "").strip().lower()
        if not ch_name:
            return False
        if any(kw in ch_name for kw in KIDS_NAME_KEYWORDS):
            return True
        all_ch = await self._get_all_channels_raw()
        kids_names = {c["title"].strip().lower() for c in all_ch if c["audience"] == "kids"}
        return ch_name in kids_names

    async def get_feed(self, tab: str = "home", limit: int = 24) -> Dict[str, Any]:
        if tab == "kids":
            now = time.monotonic()
            if self._kids_feed_cache[1] and (now - self._kids_feed_cache[0]) < 300:
                return {"tab": "kids", "signed_in": self.is_signed_in(), "requires_auth": False, "items": self._kids_feed_cache[1][:limit]}
            kids_ch = (await self.get_channels(audience="kids"))["items"]
            if not kids_ch:
                return {"tab": "kids", "signed_in": self.is_signed_in(), "requires_auth": False, "items": []}
            # Fetch latest 4 videos from up to 6 kids channels in parallel and interleave cleanly
            per_ch = await asyncio.gather(
                *(self.get_channel_videos(c["channel_id"], limit=6) for c in kids_ch[:6]),
                return_exceptions=True,
            )
            lists = [r["items"] for r in per_ch if isinstance(r, dict) and r.get("items")]
            merged: List[Dict[str, Any]] = []
            seen: set = set()
            for idx in range(6):
                for lst in lists:
                    if idx < len(lst):
                        it = lst[idx]
                        if it["video_id"] not in seen:
                            seen.add(it["video_id"])
                            merged.append(it)
            self._kids_feed_cache = (now, merged)
            return {"tab": "kids", "signed_in": self.is_signed_in(), "requires_auth": False, "items": merged[:limit]}

        browse_map = {
            "subscriptions": "FEsubscriptions",
            "home": "FEwhat_to_watch",
            "watch_later": "VLWL",
            "history": "FEhistory",
        }
        if tab in ("subscriptions", "watch_later", "history") and not self.is_signed_in():
            return {"tab": tab, "signed_in": False, "requires_auth": True, "items": []}

        browse_id = browse_map.get(tab, "FEwhat_to_watch")
        data = await self._innertube_post("browse", {"browseId": browse_id}, use_tv=self.is_signed_in())
        raw_vids: List[Dict[str, Any]] = []
        extract_videos_from_innertube(data, raw_vids, set(), limit * 2 if tab == "subscriptions" else limit, skip_shorts=True)
        # Keep Adults Subscriptions strictly free of Kids channels!
        if tab == "subscriptions" and raw_vids:
            adults_only: List[Dict[str, Any]] = []
            for v in raw_vids:
                if not await self._is_kids_video(v):
                    adults_only.append(v)
                if len(adults_only) >= limit:
                    break
            raw_vids = adults_only
        # Signed-out YouTube home often returns an empty sign-in prompt; fall back cleanly to Explore/Trending
        if not raw_vids and tab == "home" and not self.is_signed_in():
            for fb_id in ("FEtrending", "FEexplore"):
                try:
                    fb_data = await self._innertube_post("browse", {"browseId": fb_id}, use_tv=False)
                    extract_videos_from_innertube(fb_data, raw_vids, set(), limit, skip_shorts=True)
                    if raw_vids:
                        break
                except Exception:
                    continue
        return {
            "tab": tab,
            "signed_in": self.is_signed_in(),
            "requires_auth": False,
            "items": [self._to_unified_card(v) for v in raw_vids[:limit]],
        }

    async def search(self, query: str, limit: int = 12) -> List[Dict[str, Any]]:
        q = query.strip()
        if not q:
            return []
        data = await self._innertube_post("search", {"query": q}, use_tv=False)
        raw_vids: List[Dict[str, Any]] = []
        extract_videos_from_innertube(data, raw_vids, set(), limit)
        return [self._to_unified_card(v) for v in raw_vids]

    def get_continue_watching(self, limit: int = 10) -> List[Dict[str, Any]]:
        prog = self._state.get("youtube_progress", {})
        items = []
        for vid, info in prog.items():
            dur = int(info.get("duration_ms") or 0)
            off = int(info.get("offset_ms") or 0)
            if off < 10_000 or (dur > 0 and off >= int(dur * 0.92)):
                continue
            items.append((float(info.get("updated_at") or 0), vid, info))
        items.sort(key=lambda x: x[0], reverse=True)
        out = []
        for _, vid, info in items[:limit]:
            out.append(
                self._to_unified_card(
                    {
                        "video_id": vid,
                        "title": info.get("title") or vid,
                        "channel": info.get("channel") or "",
                        "duration_ms": info.get("duration_ms") or 0,
                        "thumb_raw": info.get("thumb_raw") or f"https://i.ytimg.com/vi/{vid}/hqdefault.jpg",
                    }
                )
            )
        return out

    def record_progress(
        self,
        video_id: str,
        time_ms: int,
        duration_ms: int,
        title: str = "",
        channel: str = "",
    ) -> Dict[str, Any]:
        prog = self._state.setdefault("youtube_progress", {})
        if duration_ms > 0 and time_ms >= int(duration_ms * 0.92):
            prog.pop(video_id, None)
        else:
            prev = prog.get(video_id, {})
            prog[video_id] = {
                "title": title or prev.get("title") or video_id,
                "channel": channel or prev.get("channel") or "",
                "duration_ms": int(duration_ms or prev.get("duration_ms") or 0),
                "offset_ms": max(0, int(time_ms)),
                "thumb_raw": prev.get("thumb_raw") or f"https://i.ytimg.com/vi/{video_id}/hqdefault.jpg",
                "updated_at": time.time(),
            }
            # Keep bounded to last 80 in-progress videos
            if len(prog) > 80:
                oldest = sorted(prog.items(), key=lambda kv: kv[1].get("updated_at", 0))[:-80]
                for k, _ in oldest:
                    prog.pop(k, None)
        self._save_state()
        return {"ok": True}

    # ── SponsorBlock ──

    async def get_sponsorblock_markers(self, video_id: str) -> List[Dict[str, Any]]:
        if not self.cfg.sponsorblock_enabled:
            return []
        now = time.monotonic()
        cached = self._sb_cache.get(video_id)
        if cached and (now - cached[0]) < 1800:
            return cached[1]
        try:
            cats = json.dumps(list(self.cfg.sponsorblock_categories))
            async with self.session.get(
                SPONSORBLOCK_API,
                params={"videoID": video_id, "categories": cats},
                timeout=aiohttp.ClientTimeout(total=1.8),
            ) as r:
                if r.status != 200:
                    self._sb_cache[video_id] = (now, [])
                    return []
                segs = await r.json(content_type=None)
        except Exception:
            return []

        markers = []
        for seg in segs if isinstance(segs, list) else []:
            span = seg.get("segment")
            if not isinstance(span, list) or len(span) != 2:
                continue
            cat = str(seg.get("category") or "sponsor")
            markers.append(
                {
                    "type": cat,
                    "start_ms": int(float(span[0]) * 1000),
                    "end_ms": int(float(span[1]) * 1000),
                    "action": "auto_skip" if cat in ("sponsor", "selfpromo", "interaction") else "prompt_skip",
                }
            )
        markers.sort(key=lambda m: m["start_ms"])
        self._sb_cache[video_id] = (now, markers)
        return markers

    # ── yt-dlp Stream Extraction & Caption Resolution ──

    def _extract_ytdlp_sync(self, video_id: str, max_height: int = 1080) -> Dict[str, Any]:
        import yt_dlp

        ydl_opts = {
            "quiet": True,
            "no_warnings": True,
            "skip_download": True,
            "noplaylist": True,
            "writesubtitles": False,
            "extract_flat": False,
            # Prefer TV hardware-native H.264/VP9 + AAC/M4A for zero-reencode copy remux!
            "format": (
                f"bestvideo[vcodec^=avc1][height<={max_height}]+bestaudio[acodec^=mp4a]/"
                f"bestvideo[vcodec~='^(avc1|vp0?9)'][height<={max_height}]+bestaudio/"
                f"best[height<={max_height}]/best"
            ),
        }
        url = f"https://www.youtube.com/watch?v={video_id}"
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(url, download=False)
        if not info:
            raise RuntimeError("yt-dlp returned empty info")

        req_fmts = info.get("requested_formats") or []
        if len(req_fmts) == 2:
            vfmt, afmt = req_fmts[0], req_fmts[1]
            mode = "remux_dash_copy"
            video_url = vfmt.get("url", "")
            audio_url = afmt.get("url", "")
            vcodec = str(vfmt.get("vcodec") or "")
            acodec = str(afmt.get("acodec") or "")
            width = int(vfmt.get("width") or 0)
            height = int(vfmt.get("height") or 0)
        else:
            mode = "direct_muxed"
            video_url = info.get("url", "")
            audio_url = ""
            vcodec = str(info.get("vcodec") or "")
            acodec = str(info.get("acodec") or "")
            width = int(info.get("width") or 0)
            height = int(info.get("height") or 0)

        # Collect available caption tracks (manual preferred over auto)
        captions = []
        for is_auto, key in ((False, "subtitles"), (True, "automatic_captions")):
            sub_map = info.get(key) or {}
            for lang in ("en", "en-US", "es", "es-419", "es-ES"):
                tracks = sub_map.get(lang)
                if not tracks:
                    continue
                chosen = next((t for t in tracks if t.get("ext") == "json3"), None) or next(
                    (t for t in tracks if t.get("ext") == "vtt"), None
                )
                if chosen and chosen.get("url"):
                    captions.append(
                        {
                            "lang": lang,
                            "ext": chosen["ext"],
                            "auto": is_auto,
                            "label": f"{lang.upper()} {'(Auto)' if is_auto else ''}".strip(),
                            "url": chosen["url"],
                        }
                    )

        return {
            "video_id": video_id,
            "title": str(info.get("title") or video_id),
            "channel": str(info.get("uploader") or info.get("channel") or ""),
            "duration_ms": int((info.get("duration") or 0) * 1000),
            "is_live": bool(info.get("is_live")),
            "thumb_raw": str(info.get("thumbnail") or f"https://i.ytimg.com/vi/{video_id}/hqdefault.jpg"),
            "mode": mode,
            "video_url": video_url,
            "audio_url": audio_url,
            "vcodec": vcodec,
            "acodec": acodec,
            "width": width,
            "height": height,
            "http_headers": info.get("http_headers") or {},
            "captions": captions,
        }

    async def get_resolved_stream_info(self, video_id: str, max_height: int = 1080) -> Dict[str, Any]:
        cache_key = f"{video_id}:{max_height}"
        now = time.monotonic()
        cached = self._stream_cache.get(cache_key)
        if cached and (now - cached[0]) < 1200:
            return cached[1]
        loop = asyncio.get_running_loop()
        info = await loop.run_in_executor(None, self._extract_ytdlp_sync, video_id, max_height)
        self._stream_cache[cache_key] = (now, info)
        return info

    async def resolve_playback(
        self,
        video_id: str,
        max_height: int = 1080,
        start_ms: int = 0,
        container: str = "ts",
    ) -> Dict[str, Any]:
        # Short-circuit if ytdl-sub ALREADY archived this video locally!
        local_map = self._scan_ytdl_sub_index()
        local_path = local_map.get(video_id)

        sb_task = asyncio.create_task(self.get_sponsorblock_markers(video_id))
        try:
            info = await self.get_resolved_stream_info(video_id, max_height=max_height)
            drm_fallback = False
            error_msg = ""
        except Exception as exc:
            msg = str(exc)
            drm_fallback = True
            error_msg = msg[:240]
            info = {
                "video_id": video_id,
                "title": video_id,
                "channel": "",
                "duration_ms": 0,
                "mode": "fallback_stock_youtube",
                "captions": [],
            }

        markers = await sb_task
        prog = self._state.get("youtube_progress", {}).get(video_id, {})
        resume_ms = int(start_ms or prog.get("offset_ms") or 0)

        if local_path and os.path.isfile(local_path):
            stream_url = f"{self.cfg.public_base_url}/v1/stream/youtube-local/{video_id}"
            play_mode = "local_ytdl_sub_direct"
            drm_fallback = False
        elif not drm_fallback:
            stream_url = (
                f"{self.cfg.public_base_url}/v1/stream/youtube/{video_id}"
                f"?max_height={max_height}&container={container}&start_ms={max(0, resume_ms)}"
            )
            play_mode = info["mode"]
        else:
            stream_url = ""
            play_mode = "fallback_stock_youtube"

        captions = info.get("captions") or []
        subtitle_url = (
            f"{self.cfg.public_base_url}/v1/subtitles/youtube/{video_id}?lang={captions[0]['lang']}"
            if captions
            else ""
        )
        return {
            "id": f"yt:{video_id}",
            "source": "youtube",
            "video_id": video_id,
            "title": info.get("title", video_id),
            "subtitle": info.get("channel", ""),
            "duration_ms": int(info.get("duration_ms") or 0),
            "resume_ms": resume_ms,
            "stream_url": stream_url,
            "subtitle_url": subtitle_url,
            "caption_tracks": [
                {"lang": c["lang"], "label": c["label"], "auto": c["auto"]} for c in captions
            ],
            "decision": {
                "mode": play_mode,
                "video_codec": info.get("vcodec", ""),
                "audio_codec": info.get("acodec", ""),
                "width": info.get("width", 0),
                "height": info.get("height", 0),
                "fallback_stock_youtube": drm_fallback,
                "error": error_msg,
            },
            "markers": markers,
        }

    async def get_subtitle_cues(self, video_id: str, lang: str = "en") -> Dict[str, Any]:
        cache_key = hashlib.sha256(f"yt|{video_id}|{lang}".encode()).hexdigest()
        cached = load_cached_cues(self.cfg.sub_cache_dir, cache_key)
        if cached is not None:
            return cached
        info = await self.get_resolved_stream_info(video_id)
        captions = info.get("captions") or []
        track = next((c for c in captions if c["lang"] == lang), None) or (captions[0] if captions else None)
        if not track:
            return {"format": "cues_v1", "lang": lang, "label": "None", "count": 0, "cues": []}
        async with self.session.get(
            track["url"],
            headers={"User-Agent": YT_WEB_UA},
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            r.raise_for_status()
            raw = await r.text(errors="replace")
        cues = parse_any_subtitle(raw, codec_hint=track["ext"])
        payload = {
            "format": "cues_v1",
            "lang": track["lang"],
            "label": track["label"],
            "codec": track["ext"],
            "count": len(cues),
            "cues": cues,
        }
        save_cached_cues(self.cfg.sub_cache_dir, cache_key, payload)
        return payload

    def get_local_archived_path(self, video_id: str) -> Optional[Path]:
        p = self._scan_ytdl_sub_index().get(video_id)
        return Path(p) if p and os.path.isfile(p) else None
