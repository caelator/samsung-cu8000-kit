"""Living Room Media Gateway HTTP server (LAN-only, port 8765).

Serves compact flat JSON + pre-baked Mali 0x5ca1ab13 ASTC textures + normalized
SRT/YouTube subtitles + zero-copy/zero-reencode streams to the Samsung TV
Tier 4 bare-metal C LivingRoomHub, plus a mobile /companion web console.
"""
from __future__ import annotations

import asyncio
import logging
import shutil
import time
from pathlib import Path
from typing import Any, Dict, List

import aiohttp
from aiohttp import web

from .astc_baker import AstcBaker
from .config import GatewayConfig, load_config
from .jellyseerr_client import JellyseerrClient
from .plex_client import PlexClient
from .youtube_client import YouTubeClient

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(name)s: %(message)s")
log = logging.getLogger("gateway")

COMPANION_FILE = Path(__file__).with_name("companion.html")


def _json_resp(data: Any, status: int = 200) -> web.Response:
    return web.json_response(data, status=status)


async def _stream_ffmpeg_process(
    request: web.Request,
    cmd: List[str],
    content_type: str,
) -> web.StreamResponse:
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    resp = web.StreamResponse(
        status=200,
        headers={
            "Content-Type": content_type,
            "Cache-Control": "no-store",
            "Accept-Ranges": "none",
        },
    )
    await resp.prepare(request)
    assert proc.stdout is not None
    try:
        while True:
            chunk = await proc.stdout.read(65536)
            if not chunk:
                break
            await resp.write(chunk)
    except (ConnectionError, asyncio.CancelledError, web.HTTPException):
        pass
    finally:
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                proc.kill()
        try:
            await resp.write_eof()
        except Exception:
            pass
    return resp


def create_app(cfg: GatewayConfig | None = None) -> web.Application:
    cfg = cfg or load_config()
    app = web.Application()
    app["cfg"] = cfg
    app["started_at"] = time.time()

    async def on_startup(a: web.Application) -> None:
        conn = aiohttp.TCPConnector(limit=64, ttl_dns_cache=300)
        session = aiohttp.ClientSession(connector=conn)
        a["session"] = session
        a["astc"] = AstcBaker(cfg, session)
        a["plex"] = PlexClient(cfg, session)
        a["youtube"] = YouTubeClient(cfg, session)
        a["jellyseerr"] = JellyseerrClient(cfg, session)
        a["swr_cache"] = {}
        a["warm_inflight"] = False

    async def on_cleanup(a: web.Application) -> None:
        session: aiohttp.ClientSession | None = a.get("session")
        if session:
            await session.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)

    # ── Health & Companion UI ──

    async def handle_health(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        seerr: JellyseerrClient = request.app["jellyseerr"]
        yt: YouTubeClient = request.app["youtube"]
        astc: AstcBaker = request.app["astc"]
        plex_h, seerr_h = await asyncio.gather(plex.health(), seerr.health())
        return _json_resp(
            {
                "ok": True,
                "service": "living-room-gateway",
                "uptime_s": int(time.time() - request.app["started_at"]),
                "ffmpeg_available": bool(shutil.which("ffmpeg")),
                "plex": plex_h,
                "jellyseerr": seerr_h,
                "youtube": yt.health(),
                "astc": astc.stats(),
            }
        )

    async def handle_companion(request: web.Request) -> web.Response:
        return web.FileResponse(COMPANION_FILE)

    async def handle_root(request: web.Request) -> web.Response:
        raise web.HTTPFound("/companion")

    # ── Unified Home & Kids Hubs ──

    async def _prewarm_cards(a: web.Application, cards: List[Dict[str, Any]], max_items: int = 18) -> None:
        if a.get("warm_inflight"):
            return
        a["warm_inflight"] = True
        try:
            baker: AstcBaker = a["astc"]
            sem = asyncio.Semaphore(4)

            async def bake_one(url: str) -> None:
                if not url or "/v1/art/" not in url:
                    return
                try:
                    from urllib.parse import parse_qs, urlparse
                    pu = urlparse(url)
                    preset = pu.path.rsplit("/", 1)[-1]
                    src = (parse_qs(pu.query).get("src") or [""])[0]
                    if not src:
                        return
                    async with sem:
                        await baker.get_artwork(preset, src, fmt="astc")
                except Exception:
                    pass

            urls = []
            seen = set()
            for c in cards:
                u = c.get("art_astc") or c.get("poster_astc") or c.get("thumb_astc") or ""
                if u and u not in seen:
                    seen.add(u)
                    urls.append(u)
                if len(urls) >= max_items:
                    break
            await asyncio.gather(*(bake_one(u) for u in urls))
        finally:
            a["warm_inflight"] = False

    async def _build_home(a: web.Application) -> Dict[str, Any]:
        plex: PlexClient = a["plex"]
        yt: YouTubeClient = a["youtube"]
        movie_sec = cfg.main_movie_sections[0]
        tv_sec = cfg.main_tv_sections[0]

        async def safe(coro, default):
            try:
                return await coro
            except Exception as exc:
                log.warning("Home sub-fetch failed: %s", exc)
                return default

        sections, plex_cw, recent_movies, recent_tv, yt_feed = await asyncio.gather(
            safe(plex.get_sections(), []),
            safe(plex.get_continue_watching(limit=14, kids_only=False), []),
            safe(plex.get_recently_added(movie_sec, limit=14, card_art="poster"), []),
            safe(plex.get_recently_added(tv_sec, limit=14, card_art="poster"), []),
            safe(
                yt.get_feed("subscriptions" if yt.is_signed_in() else "home", limit=14),
                {"tab": "home", "signed_in": yt.is_signed_in(), "items": []},
            ),
        )
        yt_cw = yt.get_continue_watching(limit=6)
        unified_cw = plex_cw + yt_cw
        payload = {
            "sections": sections,
            "continue_watching": unified_cw[:18],
            "youtube_row": yt_feed.get("items", []),
            "youtube_signed_in": yt.is_signed_in(),
            "recent_movies": recent_movies,
            "recent_tv": recent_tv,
        }
        a["swr_cache"]["home"] = (time.monotonic(), payload)
        asyncio.create_task(
            _prewarm_cards(a, payload["continue_watching"][:8] + payload["youtube_row"][:8] + payload["recent_movies"][:6])
        )
        return payload

    async def handle_home(request: web.Request) -> web.Response:
        fresh = request.query.get("refresh", "0") in ("1", "true")
        cache = request.app["swr_cache"].get("home")
        now = time.monotonic()
        if not fresh and cache:
            ts, payload = cache
            age = now - ts
            if age < 30:
                return _json_resp(payload)
            if age < 180:
                # Stale-while-revalidate: instant <2ms TV response, refresh in background!
                asyncio.create_task(_build_home(request.app))
                return _json_resp(payload)
        return _json_resp(await _build_home(request.app))

    async def handle_kids(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        yt: YouTubeClient = request.app["youtube"]
        kids_movie_sec = cfg.kids_section_ids[0] if len(cfg.kids_section_ids) >= 1 else 5
        kids_tv_sec = cfg.kids_section_ids[1] if len(cfg.kids_section_ids) >= 2 else 6

        async def safe(coro, default):
            try:
                return await coro
            except Exception as exc:
                log.warning("Kids sub-fetch failed: %s", exc)
                return default

        cw, kids_movies, kids_tv, recent_kids_tv, kids_yt_ch = await asyncio.gather(
            safe(plex.get_continue_watching(limit=12, kids_only=True), []),
            safe(plex.get_section_items(kids_movie_sec, start=0, size=60, sort="titleSort:asc"), {"items": []}),
            safe(plex.get_section_items(kids_tv_sec, start=0, size=80, sort="titleSort:asc"), {"items": []}),
            safe(plex.get_recently_added(kids_tv_sec, limit=14, card_art="thumb"), []),
            safe(yt.get_channels(audience="kids"), {"items": []}),
        )
        yt_ch_items = kids_yt_ch.get("items", [])
        if yt_ch_items:
            asyncio.create_task(_prewarm_cards(request.app, yt_ch_items[:12]))
        return _json_resp(
            {
                "pin_locked": False,
                "continue_watching": cw,
                "kids_youtube_channels": yt_ch_items,
                "recent_kids_episodes": recent_kids_tv,
                "kids_movies": kids_movies.get("items", []),
                "kids_tv": kids_tv.get("items", []),
            }
        )

    # ── Plex Endpoints ──

    async def handle_plex_sections(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        return _json_resp({"sections": await plex.get_sections()})

    async def handle_plex_section_items(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        sid = int(request.match_info["section_id"])
        start = int(request.query.get("start", "0"))
        size = int(request.query.get("size", "40"))
        sort = request.query.get("sort", "addedAt:desc")
        unwatched = request.query.get("unwatched", "0") in ("1", "true", "yes")
        letter = request.query.get("letter", "")
        return _json_resp(
            await plex.get_section_items(sid, start=start, size=size, sort=sort, unwatched=unwatched, letter=letter)
        )

    async def handle_plex_item(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        try:
            return _json_resp(await plex.get_item_detail(rk))
        except KeyError:
            return _json_resp({"error": "not_found"}, status=404)

    async def handle_plex_children(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        return _json_resp({"rating_key": rk, "items": await plex.get_children(rk)})

    async def handle_plex_playback(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        a_id = int(request.query["audio_id"]) if "audio_id" in request.query else None
        s_id = int(request.query["sub_id"]) if "sub_id" in request.query else None
        force = request.query.get("force_transcode", "0") in ("1", "true")
        start_ms = int(request.query.get("start_ms", "0"))
        return _json_resp(
            await plex.resolve_playback(rk, audio_stream_id=a_id, sub_stream_id=s_id, force_transcode=force, start_ms=start_ms)
        )

    async def handle_plex_timeline(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        body = await request.json()
        return _json_resp(
            await plex.report_timeline(
                rating_key=str(body["rating_key"]),
                state=str(body.get("state", "playing")),
                time_ms=int(body.get("time_ms", 0)),
                duration_ms=int(body.get("duration_ms", 0)),
            )
        )

    async def handle_stream_plex_direct(request: web.Request) -> web.StreamResponse:
        """Zero-copy HTTP Range proxy to local Plex media part (keeps token off TV)."""
        plex: PlexClient = request.app["plex"]
        part_key = request.query.get("part_key", "")
        if not part_key.startswith("/library/parts/"):
            return _json_resp({"error": "invalid part_key"}, status=400)
        headers = plex._headers()
        if "Range" in request.headers:
            headers["Range"] = request.headers["Range"]
        url = f"{cfg.plex_url}{part_key}"
        async with request.app["session"].get(
            url, headers=headers, timeout=aiohttp.ClientTimeout(total=None, sock_connect=10)
        ) as upstream:
            resp_headers = {}
            for hk in ("Content-Type", "Content-Length", "Content-Range", "Accept-Ranges", "ETag", "Last-Modified"):
                if hk in upstream.headers:
                    resp_headers[hk] = upstream.headers[hk]
            resp = web.StreamResponse(status=upstream.status, headers=resp_headers)
            await resp.prepare(request)
            try:
                async for chunk in upstream.content.iter_chunked(131072):
                    await resp.write(chunk)
            except (ConnectionResetError, asyncio.CancelledError):
                pass
            try:
                await resp.write_eof()
            except Exception:
                pass
            return resp

    async def handle_stream_plex_remux(request: web.Request) -> web.StreamResponse:
        """Audio-only Direct Stream (video untouched, audio -> EAC3 5.1 / AAC) for DTS/TrueHD."""
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        audio_idx = int(request.query.get("audio_index", "1"))
        acodec = request.query.get("acodec", "eac3")
        if acodec not in ("eac3", "aac", "ac3"):
            acodec = "eac3"
        channels = min(6, max(2, int(request.query.get("channels", "6"))))
        start_ms = max(0, int(request.query.get("start_ms", "0")))
        media_path = await plex.get_local_media_file(rk)
        if not media_path:
            return _json_resp({"error": "local_media_unavailable"}, status=404)
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return _json_resp({"error": "ffmpeg_missing"}, status=500)
        cmd = [ffmpeg, "-nostdin", "-v", "error"]
        if start_ms > 0:
            cmd += ["-ss", f"{start_ms / 1000.0:.3f}"]
        cmd += [
            "-i", str(media_path),
            "-map", "0:v:0",
            "-map", f"0:{audio_idx}",
            "-c:v", "copy",
            "-c:a", acodec,
            "-ac", str(channels),
            "-b:a", "640k" if acodec == "eac3" else "256k",
            "-f", "mpegts",
            "pipe:1",
        ]
        return await _stream_ffmpeg_process(request, cmd, "video/mp2t")

    async def handle_stream_plex_transcode(request: web.Request) -> web.StreamResponse:
        """H.264 8-bit SDR + EAC3/AAC fallback transcode for the <0.2% edge cases (10-bit H.264, DV-only, MPEG4, bitmap subs)."""
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        start_ms = max(0, int(request.query.get("start_ms", "0")))
        media_path = await plex.get_local_media_file(rk)
        if not media_path:
            return _json_resp({"error": "local_media_unavailable"}, status=404)
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return _json_resp({"error": "ffmpeg_missing"}, status=500)
        cmd = [ffmpeg, "-nostdin", "-v", "error"]
        if start_ms > 0:
            cmd += ["-ss", f"{start_ms / 1000.0:.3f}"]
        cmd += [
            "-i", str(media_path),
            "-map", "0:v:0",
            "-map", "0:a:0?",
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-crf", "19",
            "-pix_fmt", "yuv420p",
            "-c:a", "aac",
            "-b:a", "256k",
            "-f", "mpegts",
            "pipe:1",
        ]
        return await _stream_ffmpeg_process(request, cmd, "video/mp2t")

    async def handle_plex_subtitle(request: web.Request) -> web.Response:
        plex: PlexClient = request.app["plex"]
        rk = request.match_info["rating_key"]
        sid = int(request.match_info["stream_id"])
        try:
            return _json_resp(await plex.get_subtitle_cues(rk, sid))
        except KeyError:
            return _json_resp({"error": "subtitle_not_found"}, status=404)

    # ── YouTube Endpoints ──

    async def handle_yt_auth_start(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        try:
            return _json_resp(await yt.start_device_auth())
        except Exception as exc:
            return _json_resp({"error": str(exc)}, status=502)

    async def handle_yt_auth_status(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        return _json_resp(await yt.poll_device_auth())

    async def handle_yt_auth_logout(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        return _json_resp(yt.logout())

    async def handle_yt_feed(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        tab = request.match_info["tab"]
        limit = int(request.query.get("limit", "24"))
        return _json_resp(await yt.get_feed(tab, limit=limit))

    async def handle_yt_channels(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        audience = request.query.get("audience", "adults")
        category = request.query.get("category", "all")
        query = request.query.get("q", "")
        res = await yt.get_channels(audience=audience, category=category, query=query)
        if res.get("items"):
            asyncio.create_task(_prewarm_cards(request.app, res["items"][:16]))
        return _json_resp(res)

    async def handle_yt_channel_videos(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        cid = request.match_info["channel_id"]
        limit = int(request.query.get("limit", "32"))
        try:
            res = await yt.get_channel_videos(cid, limit=limit)
            if res.get("items"):
                asyncio.create_task(_prewarm_cards(request.app, res["items"][:12]))
            return _json_resp(res)
        except ValueError as exc:
            return _json_resp({"error": str(exc)}, status=400)
        except Exception as exc:
            log.warning("YouTube channel %s fetch failed: %s", cid, exc)
            return _json_resp({"error": "channel_fetch_failed"}, status=502)

    async def handle_yt_channel_classify(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        body = await request.json()
        try:
            res = yt.update_channel_prefs(
                channel_id=str(body["channel_id"]),
                audience=body.get("audience"),
                favorite=body.get("favorite"),
                category=body.get("category"),
            )
            request.app["swr_cache"].clear()
            return _json_resp(res)
        except Exception as exc:
            return _json_resp({"ok": False, "error": str(exc)}, status=400)

    async def handle_yt_channel_add(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        body = await request.json()
        try:
            res = await yt.add_custom_channel(
                url_or_handle=str(body.get("url_or_handle") or body.get("handle") or ""),
                audience=str(body.get("audience") or "kids"),
            )
            request.app["swr_cache"].clear()
            return _json_resp(res)
        except Exception as exc:
            return _json_resp({"ok": False, "error": str(exc)}, status=400)

    async def handle_yt_playback(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        vid = request.match_info["video_id"]
        max_h = int(request.query.get("max_height", "1080"))
        start_ms = int(request.query.get("start_ms", "0"))
        container = request.query.get("container", "ts")
        return _json_resp(await yt.resolve_playback(vid, max_height=max_h, start_ms=start_ms, container=container))

    async def handle_yt_progress(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        body = await request.json()
        return _json_resp(
            yt.record_progress(
                video_id=str(body["video_id"]),
                time_ms=int(body.get("time_ms", 0)),
                duration_ms=int(body.get("duration_ms", 0)),
                title=str(body.get("title", "")),
                channel=str(body.get("channel", "")),
            )
        )

    async def handle_yt_subtitle(request: web.Request) -> web.Response:
        yt: YouTubeClient = request.app["youtube"]
        vid = request.match_info["video_id"]
        lang = request.query.get("lang", "en")
        return _json_resp(await yt.get_subtitle_cues(vid, lang=lang))

    async def handle_stream_yt(request: web.Request) -> web.StreamResponse:
        """Zero-re-encode ffmpeg DASH video+audio copy remuxer for YouTube."""
        yt: YouTubeClient = request.app["youtube"]
        vid = request.match_info["video_id"]
        max_h = int(request.query.get("max_height", "1080"))
        start_ms = max(0, int(request.query.get("start_ms", "0")))
        container = request.query.get("container", "ts")
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return _json_resp({"error": "ffmpeg_missing"}, status=500)

        info = await yt.get_resolved_stream_info(vid, max_height=max_h)
        v_url = info.get("video_url", "")
        a_url = info.get("audio_url", "")
        if not v_url:
            return _json_resp({"error": "stream_unavailable"}, status=502)

        seek_args = ["-ss", f"{start_ms / 1000.0:.3f}"] if start_ms > 0 else []
        ua = str((info.get("http_headers") or {}).get("User-Agent") or "Mozilla/5.0")
        vcodec_src = info.get("vcodec", "").lower()
        v_bsf = ["-bsf:v", "h264_mp4toannexb"] if ("avc" in vcodec_src or "h264" in vcodec_src) else []
        # Keep video 100% zero-reencode copy; encode stereo audio to clean ADTS AAC (0.1% i5 CPU)
        # so MPEG-TS PTS/DTS sync starts cleanly at 0ms on Samsung Tizen hardware player!
        in_opts = [
            "-probesize", "262144", "-analyzeduration", "800000",
            "-user_agent", ua,
            "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "3",
        ]
        if a_url:
            cmd = [
                ffmpeg, "-nostdin", "-v", "error", "-fflags", "+genpts+discardcorrupt",
                *in_opts, *seek_args, "-i", v_url,
                *in_opts, *seek_args, "-i", a_url,
                "-map", "0:v:0", "-map", "1:a:0",
                "-c:v", "copy", *v_bsf,
                "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-ar", "48000",
                "-avoid_negative_ts", "make_zero",
            ]
        else:
            cmd = [
                ffmpeg, "-nostdin", "-v", "error", "-fflags", "+genpts+discardcorrupt",
                *in_opts, *seek_args, "-i", v_url,
                "-c:v", "copy", *v_bsf,
                "-c:a", "aac", "-b:a", "192k", "-ac", "2", "-ar", "48000",
                "-avoid_negative_ts", "make_zero",
            ]
        if container == "mp4":
            cmd += ["-movflags", "frag_keyframe+empty_moov+default_base_moof", "-f", "mp4", "pipe:1"]
            ctype = "video/mp4"
        else:
            cmd += ["-muxdelay", "0", "-muxpreload", "0", "-f", "mpegts", "pipe:1"]
            ctype = "video/mp2t"
        return await _stream_ffmpeg_process(request, cmd, ctype)

    async def handle_stream_yt_local(request: web.Request) -> web.StreamResponse:
        yt: YouTubeClient = request.app["youtube"]
        vid = request.match_info["video_id"]
        path = yt.get_local_archived_path(vid)
        if not path:
            return _json_resp({"error": "local_archive_not_found"}, status=404)
        return web.FileResponse(path)

    # ── Unified Search & Jellyseerr ──

    async def handle_search(request: web.Request) -> web.Response:
        q = request.query.get("q", "").strip()
        scope = request.query.get("scope", "all").lower()
        limit = int(request.query.get("limit", "10"))
        if not q:
            return _json_resp({"query": "", "plex": [], "youtube": [], "jellyseerr": []})

        plex: PlexClient = request.app["plex"]
        yt: YouTubeClient = request.app["youtube"]
        seerr: JellyseerrClient = request.app["jellyseerr"]

        async def safe(coro):
            try:
                return await coro
            except Exception as exc:
                log.warning("Search provider error: %s", exc)
                return []

        t_plex = safe(plex.search(q, limit=limit)) if scope in ("all", "plex") else asyncio.sleep(0, result=[])
        t_yt = safe(yt.search(q, limit=limit)) if scope in ("all", "youtube") else asyncio.sleep(0, result=[])
        t_seerr = safe(seerr.search(q, limit=limit)) if scope in ("all", "jellyseerr") else asyncio.sleep(0, result=[])
        r_plex, r_yt, r_seerr = await asyncio.gather(t_plex, t_yt, t_seerr)
        return _json_resp({"query": q, "plex": r_plex, "youtube": r_yt, "jellyseerr": r_seerr})

    async def handle_jellyseerr_request(request: web.Request) -> web.Response:
        seerr: JellyseerrClient = request.app["jellyseerr"]
        body = await request.json()
        try:
            res = await seerr.request_media(
                media_type=str(body["media_type"]),
                tmdb_id=int(body["tmdb_id"]),
                seasons=str(body.get("seasons", "all")),
            )
            return _json_resp(res, status=200 if res.get("ok") else 400)
        except Exception as exc:
            return _json_resp({"ok": False, "message": str(exc)}, status=400)

    # ── Hardware Mali ASTC Artwork Baker ──

    async def handle_art(request: web.Request) -> web.Response:
        baker: AstcBaker = request.app["astc"]
        preset = request.match_info["preset"]
        src = request.query.get("src", "")
        fmt = request.query.get("fmt", "astc").lower()
        yflip = request.query.get("yflip", "0") in ("1", "true")
        try:
            payload, ctype, hit = await baker.get_artwork(preset, src, fmt=fmt, yflip=yflip)
            return web.Response(
                body=payload,
                content_type=ctype,
                headers={
                    "X-Art-Cache": "HIT" if hit else "MISS",
                    "Cache-Control": "public, max-age=86400",
                },
            )
        except PermissionError as exc:
            return _json_resp({"error": str(exc)}, status=403)
        except ValueError as exc:
            return _json_resp({"error": str(exc)}, status=400)
        except Exception as exc:
            log.warning("Art bake failed (%s): %s", preset, exc)
            return _json_resp({"error": "art_bake_failed"}, status=502)

    app.add_routes(
        [
            web.get("/", handle_root),
            web.get("/v1/health", handle_health),
            web.get("/companion", handle_companion),
            web.get("/v1/home", handle_home),
            web.get("/v1/kids", handle_kids),
            web.get("/v1/plex/sections", handle_plex_sections),
            web.get("/v1/plex/section/{section_id}", handle_plex_section_items),
            web.get("/v1/plex/item/{rating_key}", handle_plex_item),
            web.get("/v1/plex/item/{rating_key}/children", handle_plex_children),
            web.get("/v1/playback/plex/{rating_key}", handle_plex_playback),
            web.post("/v1/playback/plex/timeline", handle_plex_timeline),
            web.get("/v1/stream/plex/{rating_key}", handle_stream_plex_direct),
            web.get("/v1/stream/plex-remux/{rating_key}", handle_stream_plex_remux),
            web.get("/v1/stream/plex-transcode/{rating_key}", handle_stream_plex_transcode),
            web.get("/v1/subtitles/plex/{rating_key}/{stream_id}", handle_plex_subtitle),
            web.post("/v1/youtube/auth/start", handle_yt_auth_start),
            web.get("/v1/youtube/auth/status", handle_yt_auth_status),
            web.post("/v1/youtube/auth/logout", handle_yt_auth_logout),
            web.get("/v1/youtube/feed/{tab}", handle_yt_feed),
            web.get("/v1/youtube/channels", handle_yt_channels),
            web.get("/v1/youtube/channel/{channel_id}", handle_yt_channel_videos),
            web.post("/v1/youtube/channels/classify", handle_yt_channel_classify),
            web.post("/v1/youtube/channels/add", handle_yt_channel_add),
            web.get("/v1/playback/youtube/{video_id}", handle_yt_playback),
            web.post("/v1/playback/youtube/progress", handle_yt_progress),
            web.get("/v1/subtitles/youtube/{video_id}", handle_yt_subtitle),
            web.get("/v1/stream/youtube/{video_id}", handle_stream_yt),
            web.get("/v1/stream/youtube-local/{video_id}", handle_stream_yt_local),
            web.get("/v1/search", handle_search),
            web.post("/v1/jellyseerr/request", handle_jellyseerr_request),
            web.get("/v1/art/{preset}", handle_art),
        ]
    )
    return app


def main() -> None:
    cfg = load_config()
    log.info(
        "Starting living-room-gateway on %s:%d (plex_token=%s, jellyseerr_key=%s)",
        cfg.host,
        cfg.port,
        "loaded" if cfg.plex_token else "missing",
        "loaded" if cfg.jellyseerr_api_key else "missing",
    )
    web.run_app(create_app(cfg), host=cfg.host, port=cfg.port, access_log=None)


if __name__ == "__main__":
    main()
