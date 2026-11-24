#!/bin/sh
# Idempotent, backed-up, reversible bloat strip for Samsung CU8000 (Tizen 9).
# Preserves: Plex, YouTube, AirPlay/DLNA/AllShare casting, HDMI/CEC, Bluetooth remote,
# Settings, Samsung Home fallback (csfs), iot-hub-control boot hook, SDB, Channel API, and all DRM.
set -u

BACKUP_DIR="/opt/usr/etc/codex-strip-backup"
CACHE_BACKUP="$BACKUP_DIR/home-cache"
SENTINEL_DIR="/opt/usr/share/systemd/system"

mkdir -p "$BACKUP_DIR" "$CACHE_BACKUP" "$SENTINEL_DIR"

# 1. Samsung's designed /opt ConditionPathExists=! sentinel off-switches
SENTINELS="
pisa-service
canalysis-daemon
com.samsung.tv.context-aware-agent
com.samsung.tv.context-aware-service
slive-provider-daemon
bms
org.tizen.voice-app
org.tizen.voice-client
com.samsung.tv.bixby-prov
com.samsung.tv.ondevice-voice
com.samsung.tv.ghdaemon
org.tizen.tts-engine-neural
"

sentinels_created=0
for s in $SENTINELS; do
    if [ ! -e "$SENTINEL_DIR/$s" ]; then
        touch "$SENTINEL_DIR/$s" 2>/dev/null && chmod 644 "$SENTINEL_DIR/$s" 2>/dev/null && sentinels_created=$((sentinels_created + 1))
    fi
done

# 2. Back up, uninstall, clean apps_rw, and path-block the 25 verified bloat packages
PACKAGES="
HEPsqFNie0
com.samsung.tv.tvplus
com.samsung.tv.tvPlusDeeplinker
com.samsung.tv.targeted-ad-service
com.samsung.tv.acr-service-app
com.samsung.tv.AccountNudgeService
com.samsung.tv.ghdaemon
com.samsung.tv.gamehome
com.samsung.tv.bixby-prov
com.samsung.tv.ondevice-voice
com.samsung.tv.vif
com.samsung.tv.utterance-recommender-service
org.tizen.voice-client
org.tizen.voice-app
com.samsung.tv.alexa-client
com.samsung.tv.bixbycapsuleviewer
org.tizen.tts-engine-neural
com.samsung.tizen.smartthings-client
com.samsung.tizen.smartthings-thing
com.samsung.tv.smartthingsfind-daemon
com.samsung.tv.tvsensor-iot-service
com.samsung.tv.MultiControl-Service
com.samsung.tv.health-mde-service
com.samsung.tv.mde-framework
com.samsung.tv.tap-service-agent
"

# Stop running TV Plus app cleanly first
app_launcher --kill=HEPsqFNie0.tvplusstandalone >/dev/null 2>&1 || true

pkg_removed=0
pkg_blocked=0
pkg_backed_up=0
for pkg in $PACKAGES; do
    app_dir="/opt/usr/apps/$pkg"
    if [ -d "$app_dir" ]; then
        if [ ! -f "$BACKUP_DIR/${pkg}.tar" ]; then
            tar -cf "$BACKUP_DIR/${pkg}.tar" -C /opt/usr/apps "$pkg" 2>/dev/null && pkg_backed_up=$((pkg_backed_up + 1))
        fi
        pkgcmd -u -n "$pkg" >/dev/null 2>&1 || true
        rm -rf "/opt/usr/home/owner/apps_rw/$pkg" "$app_dir" 2>/dev/null || true
        pkg_removed=$((pkg_removed + 1))
    fi
    # Create zero-byte read-only blocker so pkg_recovery does not reinstall on boot
    if [ ! -d "$app_dir" ] && [ ! -f "$app_dir" ]; then
        touch "$app_dir" 2>/dev/null && chmod 444 "$app_dir" 2>/dev/null && pkg_blocked=$((pkg_blocked + 1))
    fi
done

# 3. Buxton2 visual/auto-update cleanup (backs up originals once; preserves existing non-TVPLUS apps!)
BUXTON_LOG="$CACHE_BACKUP/buxton-originals.txt"
buxton_changed=0
if [ -x /usr/bin/buxton2ctl ]; then
    record_orig() {
        key="$1"
        if [ ! -f "$BUXTON_LOG" ] || ! grep -q "^${key}=" "$BUXTON_LOG" 2>/dev/null; then
            orig=$(/usr/bin/buxton2ctl get-value system "$key" 2>/dev/null | tr '\n' ' ' || true)
            printf '%s=%s\n' "$key" "$orig" >> "$BUXTON_LOG"
        fi
    }

    set_buxton_str() {
        key="$1"; val="$2"
        record_orig "$key"
        /usr/bin/buxton2ctl set-string system "$key" "$val" >/dev/null 2>&1 && buxton_changed=$((buxton_changed + 1)) || true
    }

    set_buxton_int() {
        key="$1"; val="$2"
        record_orig "$key"
        /usr/bin/buxton2ctl set-int32 system "$key" "$val" >/dev/null 2>&1 && buxton_changed=$((buxton_changed + 1)) || true
    }

    set_buxton_bool() {
        key="$1"; val="$2"
        record_orig "$key"
        /usr/bin/buxton2ctl set-bool system "$key" "$val" >/dev/null 2>&1 && buxton_changed=$((buxton_changed + 1)) || true
    }

    # Try bool/string safely matching Samsung's Tizen 9 key types
    set_buxton_str "db/eden/homeRecommendation" "false"
    set_buxton_str "db/eden/autoplay_sound_on" "False"
    set_buxton_int "db/home/mute_autoplay_sound" "1"
    set_buxton_str "db/menu/Smarthub/App_Game_Auto_Update" "OFF"
    set_buxton_str "db/menu/autorun_tv_plus_state" "disable"
    set_buxton_str "db/menu/bixby_update_item" "False"

    # Strip only TVPLUS entries from db/eden/launcher/list while preserving all other installed apps
    record_orig "db/eden/launcher/list"
    cur_list=$(/usr/bin/buxton2ctl get-value system "db/eden/launcher/list" 2>/dev/null || true)
    case "$cur_list" in
        *TVPLUS*|*tvplus*)
            # Remove comma-delimited tokens referencing TVPLUS cleanly via awk
            cleaned=$(printf '%s' "$cur_list" | awk -F',' '{
                out=""
                for (i=1; i<=NF; i++) {
                    if ($i !~ /TVPLUS|tvplus|HEPsqFNie0/) {
                        out = (out == "" ? $i : out "," $i)
                    }
                }
                print out
            }')
            if [ -n "$cleaned" ] && [ "$cleaned" != "$cur_list" ]; then
                /usr/bin/buxton2ctl set-string system "db/eden/launcher/list" "$cleaned" >/dev/null 2>&1 && buxton_changed=$((buxton_changed + 1)) || true
            fi
            ;;
    esac
fi

# 4. Back up & clean Home launcher caches + TVPLUS SQLite row
cache_cleaned=0
for cf in \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/data/.appLayerCache.1 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/data/.appLayerCache.2 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/data/.ContentsBrowserCache.1 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/data/.ContentsBrowserCache.2 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.csfs/data/vp.cache.1 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.csfs/data/vp.cache.2 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.csfs/data/vppage.cache.1 \
    /opt/usr/home/owner/apps_rw/com.samsung.tv.csfs/data/vppage.cache.2; do
    if [ -f "$cf" ]; then
        bn=$(basename "$cf")
        [ -f "$CACHE_BACKUP/$bn" ] || cp -p "$cf" "$CACHE_BACKUP/$bn" 2>/dev/null || true
        rm -f "$cf" 2>/dev/null && cache_cleaned=$((cache_cleaned + 1))
    fi
done

HOME_DB="/opt/usr/home/owner/apps_rw/com.samsung.tv.home.media/shared/trusted/sqlite.db"
if [ -f "$HOME_DB" ] && command -v sqlite3 >/dev/null 2>&1; then
    [ -f "$CACHE_BACKUP/home-media-sqlite.db" ] || cp -p "$HOME_DB" "$CACHE_BACKUP/home-media-sqlite.db" 2>/dev/null || true
    sqlite3 "$HOME_DB" "DELETE FROM items WHERE uri LIKE '%TVPLUS%' OR title LIKE '%Samsung TV Plus%';" >/dev/null 2>&1 || true
fi

printf 'STRIP_OK sentinels_new=%d pkg_backed_up=%d pkg_removed=%d pkg_blocked=%d buxton=%d cache=%d\n' \
    "$sentinels_created" "$pkg_backed_up" "$pkg_removed" "$pkg_blocked" "$buxton_changed" "$cache_cleaned"
exit 0
