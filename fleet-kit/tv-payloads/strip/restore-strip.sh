#!/bin/sh
# Complete rollback for apply-strip.sh on Samsung CU8000 (Tizen 9).
set -u

BACKUP_DIR="/opt/usr/etc/codex-strip-backup"
CACHE_BACKUP="$BACKUP_DIR/home-cache"
SENTINEL_DIR="/opt/usr/share/systemd/system"

# 1. Remove Samsung /opt sentinels created by strip
for s in \
    pisa-service canalysis-daemon com.samsung.tv.context-aware-agent \
    com.samsung.tv.context-aware-service slive-provider-daemon bms \
    org.tizen.voice-app org.tizen.voice-client com.samsung.tv.bixby-prov \
    com.samsung.tv.ondevice-voice com.samsung.tv.ghdaemon org.tizen.tts-engine-neural; do
    rm -f "$SENTINEL_DIR/$s" 2>/dev/null || true
done

# 2. Remove zero-byte path blockers & restore backed-up packages
restored_pkgs=0
for pkg in \
    HEPsqFNie0 com.samsung.tv.tvplus com.samsung.tv.tvPlusDeeplinker \
    com.samsung.tv.targeted-ad-service com.samsung.tv.acr-service-app \
    com.samsung.tv.AccountNudgeService com.samsung.tv.ghdaemon com.samsung.tv.gamehome \
    com.samsung.tv.bixby-prov com.samsung.tv.ondevice-voice com.samsung.tv.vif \
    com.samsung.tv.utterance-recommender-service org.tizen.voice-client org.tizen.voice-app \
    com.samsung.tv.alexa-client com.samsung.tv.bixbycapsuleviewer org.tizen.tts-engine-neural \
    com.samsung.tizen.smartthings-client com.samsung.tizen.smartthings-thing \
    com.samsung.tv.smartthingsfind-daemon com.samsung.tv.tvsensor-iot-service \
    com.samsung.tv.MultiControl-Service com.samsung.tv.health-mde-service \
    com.samsung.tv.mde-framework com.samsung.tv.tap-service-agent; do
    app_dir="/opt/usr/apps/$pkg"
    if [ -f "$app_dir" ] && [ ! -s "$app_dir" ]; then
        chmod 644 "$app_dir" 2>/dev/null || true
        rm -f "$app_dir" 2>/dev/null || true
    fi
    if [ -f "$BACKUP_DIR/${pkg}.tar" ] && [ ! -d "$app_dir" ]; then
        tar -xf "$BACKUP_DIR/${pkg}.tar" -C /opt/usr/apps 2>/dev/null && restored_pkgs=$((restored_pkgs + 1))
    fi
done

# 3. Restore Buxton visual settings to defaults where applicable
if [ -x /usr/bin/buxton2ctl ]; then
    /usr/bin/buxton2ctl set-string system "db/eden/homeRecommendation" "true" >/dev/null 2>&1 || true
    /usr/bin/buxton2ctl set-string system "db/eden/autoplay_sound_on" "True" >/dev/null 2>&1 || true
    /usr/bin/buxton2ctl set-int32 system "db/home/mute_autoplay_sound" "0" >/dev/null 2>&1 || true
    /usr/bin/buxton2ctl set-string system "db/menu/Smarthub/App_Game_Auto_Update" "ON" >/dev/null 2>&1 || true
    /usr/bin/buxton2ctl set-string system "db/menu/autorun_tv_plus_state" "enable" >/dev/null 2>&1 || true
    /usr/bin/buxton2ctl set-string system "db/menu/bixby_update_item" "True" >/dev/null 2>&1 || true
fi

# 4. Restore Home SQLite backup if present
HOME_DB="/opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/shared/trusted/sqlite.db"
if [ -f "$CACHE_BACKUP/home-media-sqlite.db" ] && [ -d "$(dirname "$HOME_DB")" ]; then
    cp -p "$CACHE_BACKUP/home-media-sqlite.db" "$HOME_DB" 2>/dev/null || true
fi

printf 'STRIP_RESTORE_OK restored_pkgs=%d\n' "$restored_pkgs"
exit 0
