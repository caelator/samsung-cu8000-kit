"""Zero-secret runtime configuration loader for living-room-gateway.

Reads Plex and Jellyseerr credentials directly from read-only mounted files
into memory only. Secrets are never serialized into logs, reprs, or responses.
"""
from __future__ import annotations

import json
import os
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Tuple


def _parse_int_tuple(raw: str, default: Tuple[int, ...]) -> Tuple[int, ...]:
    vals = []
    for part in (raw or "").split(","):
        part = part.strip()
        if part.isdigit():
            vals.append(int(part))
    return tuple(vals) if vals else default


@dataclass
class GatewayConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    public_base_url: str = "http://127.0.0.1:8765"

    plex_url: str = "http://127.0.0.1:32400"
    plex_public_url: str = "http://127.0.0.1:32400"
    plex_prefs_path: Path = Path("/secrets/plex-preferences.xml")
    plex_token: str = field(default="", repr=False)
    plex_machine_id: str = ""

    jellyseerr_url: str = "http://127.0.0.1:5055"
    jellyseerr_settings_path: Path = Path("/secrets/jellyseerr-settings.json")
    jellyseerr_api_key: str = field(default="", repr=False)

    config_dir: Path = Path("/config")
    media_data_dir: Path = Path("/data")

    main_movie_sections: Tuple[int, ...] = (1,)
    main_tv_sections: Tuple[int, ...] = (2,)
    kids_section_ids: Tuple[int, ...] = (5, 6)

    sponsorblock_enabled: bool = True
    sponsorblock_categories: Tuple[str, ...] = (
        "sponsor",
        "selfpromo",
        "interaction",
        "intro",
        "outro",
    )
    astc_max_cache_mb: int = 2048
    client_identifier: str = "living-room-hub-cu8000"
    client_product: str = "LivingRoomHub"
    client_platform: str = "Tizen"
    client_device: str = "UN75CU8000"

    @property
    def astc_cache_dir(self) -> Path:
        return self.config_dir / "cache" / "astc"

    @property
    def sub_cache_dir(self) -> Path:
        return self.config_dir / "cache" / "subs"

    @property
    def youtube_oauth_path(self) -> Path:
        return self.config_dir / "youtube_oauth.json"

    @property
    def youtube_channels_path(self) -> Path:
        return self.config_dir / "youtube_channels.json"

    @property
    def state_path(self) -> Path:
        return self.config_dir / "state.json"

    def ensure_dirs(self) -> None:
        for d in (self.config_dir, self.astc_cache_dir, self.sub_cache_dir):
            d.mkdir(parents=True, exist_ok=True)


def _read_plex_prefs(path: Path) -> Tuple[str, str]:
    if not path.is_file():
        return "", ""
    try:
        root = ET.parse(path).getroot()
        token = root.attrib.get("PlexOnlineToken", "").strip()
        machine_id = root.attrib.get("ProcessedMachineIdentifier", "").strip()
        return token, machine_id
    except Exception:
        return "", ""


def _read_jellyseerr_key(path: Path) -> str:
    if not path.is_file():
        return ""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return str(data.get("main", {}).get("apiKey", "")).strip()
    except Exception:
        return ""


def load_config() -> GatewayConfig:
    cfg = GatewayConfig(
        host=os.environ.get("GATEWAY_HOST", "0.0.0.0"),
        port=int(os.environ.get("GATEWAY_PORT", "8765")),
        public_base_url=os.environ.get("PUBLIC_BASE_URL", "http://127.0.0.1:8765").rstrip("/"),
        plex_url=os.environ.get("PLEX_URL", "http://127.0.0.1:32400").rstrip("/"),
        plex_public_url=os.environ.get("PLEX_PUBLIC_URL", "http://127.0.0.1:32400").rstrip("/"),
        plex_prefs_path=Path(os.environ.get("PLEX_PREFS_PATH", "/secrets/plex-preferences.xml")),
        jellyseerr_url=os.environ.get("JELLYSEERR_URL", "http://127.0.0.1:5055").rstrip("/"),
        jellyseerr_settings_path=Path(
            os.environ.get("JELLYSEERR_SETTINGS_PATH", "/secrets/jellyseerr-settings.json")
        ),
        config_dir=Path(os.environ.get("CONFIG_DIR", "/config")),
        media_data_dir=Path(os.environ.get("MEDIA_DATA_DIR", "/data")),
        main_movie_sections=_parse_int_tuple(os.environ.get("MAIN_MOVIE_SECTIONS", "1"), (1,)),
        main_tv_sections=_parse_int_tuple(os.environ.get("MAIN_TV_SECTIONS", "2"), (2,)),
        kids_section_ids=_parse_int_tuple(os.environ.get("KIDS_SECTION_IDS", "5,6"), (5, 6)),
        sponsorblock_enabled=os.environ.get("SPONSORBLOCK_ENABLED", "true").lower()
        in ("1", "true", "yes", "on"),
        astc_max_cache_mb=int(os.environ.get("ASTC_MAX_CACHE_MB", "2048")),
    )

    plex_token = os.environ.get("PLEX_TOKEN", "").strip()
    plex_mid = os.environ.get("PLEX_MACHINE_ID", "").strip()
    if not plex_token or not plex_mid:
        file_token, file_mid = _read_plex_prefs(cfg.plex_prefs_path)
        cfg.plex_token = plex_token or file_token
        cfg.plex_machine_id = plex_mid or file_mid
    else:
        cfg.plex_token = plex_token
        cfg.plex_machine_id = plex_mid

    seerr_key = os.environ.get("JELLYSEERR_API_KEY", "").strip()
    cfg.jellyseerr_api_key = seerr_key or _read_jellyseerr_key(cfg.jellyseerr_settings_path)
    cfg.ensure_dirs()
    return cfg
