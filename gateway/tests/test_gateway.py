"""Unit & integration tests for living-room-gateway."""
from __future__ import annotations

import json
import struct
from pathlib import Path

import pytest
from aiohttp.test_utils import TestClient, TestServer

from app.astc_baker import ASTC_MAGIC, is_allowed_source, validate_astc_bytes
from app.config import GatewayConfig
from app.plex_client import decide_playback
from app.server import create_app
from app.subtitles import parse_any_subtitle, parse_ass, parse_srt, parse_vtt, parse_youtube_json3
from app.youtube_client import (
    classify_adult_category,
    classify_channel_audience,
    extract_channels_from_innertube,
    extract_videos_from_innertube,
    parse_duration_text_ms,
)


def test_astc_source_allowlist_blocks_ssrf():
    assert is_allowed_source("/library/metadata/123/thumb/456")
    assert is_allowed_source("/photo/:/transcode?url=abc")
    assert is_allowed_source("https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg")
    assert is_allowed_source("https://image.tmdb.org/t/p/w342/abc.jpg")
    # Disallowed / SSRF attempts
    assert not is_allowed_source("http://127.0.0.1:8080/secret")
    assert not is_allowed_source("http://192.0.2.10:8989/api")
    assert not is_allowed_source("//evil.example/a.jpg")
    assert not is_allowed_source("/library/../../etc/passwd")
    assert not is_allowed_source("file:///etc/passwd")


def test_astc_header_validation():
    # Build a synthetic valid 240x360 6x6 ASTC header + payload (40 * 60 = 2400 blocks)
    bx, by, bz = 6, 6, 1
    w, h = 240, 360
    hdr = struct.pack("<IBBB", ASTC_MAGIC, bx, by, bz)
    hdr += bytes([w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF])
    hdr += bytes([h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF])
    hdr += bytes([1, 0, 0])
    payload = hdr + (b"\x00" * (40 * 60 * 16))
    assert validate_astc_bytes(payload) == (240, 360, 6, 6)
    with pytest.raises(ValueError):
        validate_astc_bytes(b"not-astc-header-data")


def test_decide_playback_cu8000_earc_matrix():
    # 1. Standard 1080p H.264 + EAC3 5.1 MKV + SRT -> Direct Play + native cues (98%+ of library!)
    media = {"container": "mkv", "videoCodec": "h264", "audioCodec": "eac3", "audioChannels": 6, "width": 1920, "height": 1080}
    part = {"container": "mkv", "Stream": [{"streamType": 1, "codec": "h264", "bitDepth": 8}]}
    audio = {"streamType": 2, "codec": "eac3", "channels": 6}
    sub_srt = {"streamType": 3, "codec": "srt"}
    d1 = decide_playback(media, part, audio, sub_srt)
    assert d1["mode"] == "direct_play"
    assert d1["sub_mode"] == "native_cues"

    # 2. DTS 5.1 audio -> direct_stream_audio (video untouched, audio -> EAC3 5.1 for eARC AVR!)
    audio_dts = {"streamType": 2, "codec": "dca", "channels": 6}
    d2 = decide_playback(media, part, audio_dts, sub_srt)
    assert d2["mode"] == "direct_stream_audio"
    assert d2["target_audio_codec"] == "eac3"
    assert d2["sub_mode"] == "native_cues"

    # 3. H.264 10-bit anime -> transcode_video
    part_10 = {"container": "mkv", "Stream": [{"streamType": 1, "codec": "h264", "bitDepth": 10}]}
    d3 = decide_playback(media, part_10, audio, None)
    assert d3["mode"] == "transcode_video"
    assert "h264_10bit" in d3["reason"]

    # 4. HEVC HDR10 -> Direct Play
    part_hdr = {"container": "mkv", "Stream": [{"streamType": 1, "codec": "hevc", "bitDepth": 10, "colorTrc": "smpte2084"}]}
    d4 = decide_playback(media, part_hdr, audio, None)
    assert d4["mode"] == "direct_play"
    assert d4["hdr"] == "HDR10"

    # 5. HEVC Dolby Vision without HDR10 base layer -> transcode_video
    part_dv = {"container": "mkv", "Stream": [{"streamType": 1, "codec": "hevc", "bitDepth": 10, "DOVIPresent": True, "DOVIBLCompatID": 0}]}
    d5 = decide_playback(media, part_dv, audio, None)
    assert d5["mode"] == "transcode_video"

    # 6. Bitmap PGS subtitle explicitly selected -> transcode burn_in
    d6 = decide_playback(media, part, audio, {"streamType": 3, "codec": "pgs"})
    assert d6["mode"] == "transcode_video"
    assert d6["sub_mode"] == "burn_in"


def test_subtitle_parsers_srt_vtt_ass_json3():
    srt = "1\n00:00:01,500 --> 00:00:04,250\n<i>Hola</i> &amp; bienvenido!\n\n2\n00:00:05,000 --> 00:00:07,000\nLine 2\n"
    cues = parse_srt(srt)
    assert cues == [[1500, 4250, "Hola & bienvenido!"], [5000, 7000, "Line 2"]]

    vtt = "WEBVTT\n\n00:01.000 --> 00:03.500\n<c>Hello</c>\n\n00:03.550 --> 00:05.000\nHello\n"
    # Adjacent duplicate merges smoothly
    assert parse_vtt(vtt) == [[1000, 5000, "Hello"]]

    ass = "[Events]\nDialogue: 0,0:00:02.50,0:00:05.10,Default,,0,0,0,,{\\an8}Top\\NBottom\n"
    assert parse_ass(ass) == [[2500, 5100, "Top\nBottom"]]

    j3 = json.dumps({"events": [{"tStartMs": 1200, "dDurationMs": 2000, "segs": [{"utf8": "Test "}, {"utf8": "YT"}]}]})
    assert parse_youtube_json3(j3) == [[1200, 3200, "Test YT"]]
    assert parse_any_subtitle(j3, "json3") == [[1200, 3200, "Test YT"]]


def test_innertube_extractor_and_duration():
    assert parse_duration_text_ms("12:34") == (12 * 60 + 34) * 1000
    assert parse_duration_text_ms("1:02:03") == (3600 + 120 + 3) * 1000
    sample = {
        "contents": [
            {
                "videoRenderer": {
                    "videoId": "abcdefghijk",
                    "title": {"runs": [{"text": "Sample Video"}]},
                    "ownerText": {"runs": [{"text": "Demo Channel"}]},
                    "lengthText": {"simpleText": "10:05"},
                    "thumbnail": {"thumbnails": [{"url": "https://i.ytimg.com/vi/abcdefghijk/hqdefault.jpg", "width": 480}]},
                }
            }
        ]
    }
    out = []
    extract_videos_from_innertube(sample, out, set(), 10)
    assert len(out) == 1
    assert out[0]["video_id"] == "abcdefghijk"
    assert out[0]["duration_ms"] == 605_000

    # TVHTML5 tileRenderer nested lineRenderer -> lineItemRenderer format
    tv_tile = {
        "tileRenderer": {
            "contentId": "1234567890a",
            "metadata": {
                "tileMetadataRenderer": {
                    "title": {"simpleText": "TV Video"},
                    "lines": [
                        {"lineRenderer": {"items": [{"lineItemRenderer": {"text": {"runs": [{"text": "Creator"}]}}}]}},
                        {"lineRenderer": {"items": [{"lineItemRenderer": {"text": {"simpleText": "95K views"}}}, {"lineItemRenderer": {"text": {"simpleText": "•"}}}, {"lineItemRenderer": {"text": {"simpleText": "12h ago"}}}]}},
                    ],
                }
            },
            "header": {"tileHeaderRenderer": {"thumbnailOverlays": [{"thumbnailOverlayTimeStatusRenderer": {"text": {"simpleText": "8:15"}}}]}},
        }
    }
    out2 = []
    extract_videos_from_innertube(tv_tile, out2, set(), 10)
    assert len(out2) == 1
    assert out2[0]["channel"] == "Creator"
    assert "95K views" in out2[0]["meta"]
    assert out2[0]["duration_ms"] == (8 * 60 + 15) * 1000


@pytest.mark.asyncio
async def test_gateway_http_health_and_art_guard(tmp_path: Path):
    cfg = GatewayConfig(
        config_dir=tmp_path / "cfg",
        media_data_dir=tmp_path / "data",
        plex_url="http://127.0.0.1:1",
        jellyseerr_url="http://127.0.0.1:1",
    )
    cfg.ensure_dirs()
    app = create_app(cfg)
    async with TestClient(TestServer(app)) as client:
        r = await client.get("/v1/health")
        assert r.status == 200
        body = await r.json()
        assert body["ok"] is True
        assert body["service"] == "living-room-gateway"

        # Companion HTML loads
        rc = await client.get("/companion")
        assert rc.status == 200
        assert "<title>Living Room</title>" in await rc.text()

        # SSRF blocked on /v1/art
        r_bad = await client.get("/v1/art/poster?src=http://127.0.0.1:32400/secret")
        assert r_bad.status == 403

        # YouTube progress persists & shows in continue watching
        rp = await client.post(
            "/v1/playback/youtube/progress",
            json={"video_id": "abcdefghijk", "time_ms": 45000, "duration_ms": 300000, "title": "Demo", "channel": "Ch"},
        )
        assert rp.status == 200
        assert len(app["youtube"].get_continue_watching()) == 1

        # YouTube channels classification + override persistence
        assert classify_channel_audience("UCG2CL6EUjG8TVT1Tpl9nJdg", "Ms Rachel", {}) == "kids"
        assert classify_channel_audience("UCrandom12345678901234", "Bourbon Moth Woodworking", {}) == "adults"
        assert classify_adult_category("UCrandom12345678901234", "Bourbon Moth Woodworking", {}) == "woodworking"
        assert classify_adult_category("UCrandom12345678901235", "Evolving AI", {}) == "tech"

        # Classify endpoint persists user overrides cleanly
        rc2 = await client.post(
            "/v1/youtube/channels/classify",
            json={"channel_id": "UCrandom12345678901234", "audience": "kids", "favorite": True},
        )
        assert rc2.status == 200
        rc2_body = await rc2.json()
        assert rc2_body["ok"] is True
        assert rc2_body["audience"] == "kids"
        assert rc2_body["is_favorite"] is True

        # Shorts (blank-line tileRenderer) skipped when skip_shorts=True
        short_tile = {
            "tileRenderer": {
                "contentId": "short123456",
                "metadata": {"tileMetadataRenderer": {"title": {"simpleText": "A Short"}}},
            }
        }
        out_s = []
        extract_videos_from_innertube(short_tile, out_s, set(), 10, skip_shorts=True)
        assert out_s == []

        # FEchannels extractor harvests UC channel IDs + avatars
        fe_sample = {
            "channelRenderer": {
                "channelId": "UC1234567890123456789012",
                "title": {"simpleText": "Demo Woodworking"},
                "thumbnail": {"thumbnails": [{"url": "//yt3.ggpht.com/av.jpg"}]},
            }
        }
        ch_out = []
        extract_channels_from_innertube(fe_sample, ch_out, set())
        assert len(ch_out) == 1
        assert ch_out[0]["channel_id"] == "UC1234567890123456789012"
        assert ch_out[0]["avatar"] == "https://yt3.ggpht.com/av.jpg"
