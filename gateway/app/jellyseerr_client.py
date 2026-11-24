"""Loopback Jellyseerr (:5055) client for unified TV search & 1-click requests."""
from __future__ import annotations

import time
from typing import Any, Dict, List
from urllib.parse import quote

import aiohttp

from .config import GatewayConfig

TMDB_IMG_BASE = "https://image.tmdb.org/t/p/w342"
STATUS_LABELS = {
    1: "not_requested",
    2: "pending",
    3: "processing",
    4: "partially_available",
    5: "available",
}


class JellyseerrClient:
    def __init__(self, cfg: GatewayConfig, session: aiohttp.ClientSession) -> None:
        self.cfg = cfg
        self.session = session

    def _headers(self) -> Dict[str, str]:
        h = {"Accept": "application/json"}
        if self.cfg.jellyseerr_api_key:
            h["X-Api-Key"] = self.cfg.jellyseerr_api_key
        return h

    def _art_url(self, poster_path: str) -> str:
        if not poster_path:
            return ""
        src = f"{TMDB_IMG_BASE}{poster_path}"
        return f"{self.cfg.public_base_url}/v1/art/poster?src={quote(src, safe='')}"

    async def health(self) -> Dict[str, Any]:
        t0 = time.monotonic()
        try:
            async with self.session.get(
                f"{self.cfg.jellyseerr_url}/api/v1/status",
                headers=self._headers(),
                timeout=aiohttp.ClientTimeout(total=3),
            ) as r:
                ok = r.status == 200
                data = await r.json(content_type=None) if ok else {}
                return {
                    "online": ok,
                    "latency_ms": round((time.monotonic() - t0) * 1000, 1),
                    "version": data.get("version", ""),
                    "has_api_key": bool(self.cfg.jellyseerr_api_key),
                }
        except Exception as exc:
            return {"online": False, "error": str(exc), "has_api_key": bool(self.cfg.jellyseerr_api_key)}

    async def search(self, query: str, limit: int = 12) -> List[Dict[str, Any]]:
        q = query.strip()
        if not q or not self.cfg.jellyseerr_api_key:
            return []
        async with self.session.get(
            f"{self.cfg.jellyseerr_url}/api/v1/search",
            headers=self._headers(),
            params={"query": q, "page": "1", "language": "en"},
            timeout=aiohttp.ClientTimeout(total=8),
        ) as r:
            r.raise_for_status()
            data = await r.json(content_type=None)

        out: List[Dict[str, Any]] = []
        for item in data.get("results", []):
            mtype = item.get("mediaType")
            if mtype not in ("movie", "tv"):
                continue
            title = item.get("title") or item.get("name") or ""
            date_str = item.get("releaseDate") or item.get("firstAirDate") or ""
            year = int(date_str[:4]) if len(date_str) >= 4 and date_str[:4].isdigit() else 0
            media_info = item.get("mediaInfo") or {}
            status_code = int(media_info.get("status") or 1)
            status = STATUS_LABELS.get(status_code, "not_requested")
            out.append(
                {
                    "id": f"seerr:{mtype}:{item['id']}",
                    "source": "jellyseerr",
                    "media_type": mtype,
                    "tmdb_id": int(item["id"]),
                    "title": title,
                    "subtitle": f"{'Movie' if mtype == 'movie' else 'TV'} · {year}" if year else mtype.upper(),
                    "year": year,
                    "overview": str(item.get("overview") or "")[:360],
                    "status": status,
                    "can_request": status == "not_requested",
                    "poster_astc": self._art_url(str(item.get("posterPath") or "")),
                    "art_astc": self._art_url(str(item.get("posterPath") or "")),
                }
            )
            if len(out) >= limit:
                break
        return out

    async def request_media(self, media_type: str, tmdb_id: int, seasons: str = "all") -> Dict[str, Any]:
        if media_type not in ("movie", "tv"):
            raise ValueError("media_type must be 'movie' or 'tv'")
        if not self.cfg.jellyseerr_api_key:
            raise RuntimeError("Jellyseerr API key not configured")

        body: Dict[str, Any] = {"mediaType": media_type, "mediaId": int(tmdb_id)}
        if media_type == "tv":
            body["seasons"] = seasons  # "all" uses Jellyseerr's default 720p-first Sonarr profile!

        async with self.session.post(
            f"{self.cfg.jellyseerr_url}/api/v1/request",
            headers={**self._headers(), "Content-Type": "application/json"},
            json=body,
            timeout=aiohttp.ClientTimeout(total=10),
        ) as r:
            resp_data = await r.json(content_type=None)
            if r.status >= 400:
                return {
                    "ok": False,
                    "status_code": r.status,
                    "message": resp_data.get("message") or f"HTTP {r.status}",
                }
            return {
                "ok": True,
                "request_id": resp_data.get("id"),
                "status": STATUS_LABELS.get(int(resp_data.get("status") or 2), "pending"),
                "media_type": media_type,
                "tmdb_id": int(tmdb_id),
            }
