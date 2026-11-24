#!/bin/sh
# Atomically render public /run/codex-vpn/status.json (mode 0644, Smack _).
# Strictly zero credentials, tokens, or private paths.
set -eu

BASE="${CODEX_VPN_BASE:-/opt/usr/etc/codex-vpn}"
RUN="${CODEX_VPN_RUN:-/run/codex-vpn}"
mkdir -p "$RUN"
chmod 755 "$RUN" 2>/dev/null || true

# Load selected location ID (defaults to 54 = USA - Miami)
SEL_ID="54"
if [ -f "$BASE/selected-location.id" ]; then
    raw_sel=$(tr -dc '0-9' < "$BASE/selected-location.id" 2>/dev/null | head -c 6 || true)
    [ -n "$raw_sel" ] && SEL_ID="$raw_sel"
fi

# Load live runtime state (strictly public fields)
STATUS="disconnected"
AUTH_ID=""
ACTIVE_ID=""
EXIT_IP=""
COLO=""
ENDPOINT_IP=""
ERR_STAGE=""
ERR_CODE=""
ERR_MSG=""
ERR_ROLLBACK="true"

if [ -f "$RUN/state.env" ]; then
    # shellcheck disable=SC1091
    . "$RUN/state.env" 2>/dev/null || true
fi

# Helper: lookup "id|name|cc" from endpoints.tsv
loc_json() {
    lid="$1"
    if [ -z "$lid" ]; then
        printf 'null'
        return
    fi
    row=$(awk -F '\t' -v id="$lid" '$1 == id { printf "%s\t%s\t%s", $1, $2, $3; exit }' "$BASE/endpoints.tsv" 2>/dev/null || true)
    if [ -z "$row" ]; then
        printf 'null'
        return
    fi
    lname=$(printf '%s' "$row" | cut -f2 | sed 's/\\/\\\\/g; s/"/\\"/g')
    lcc=$(printf '%s' "$row" | cut -f3 | sed 's/[^A-Z]//g')
    printf '{"id":"%s","name":"%s","countryCode":"%s"}' "$lid" "$lname" "$lcc"
}

json_str_or_null() {
    val="$1"
    if [ -z "$val" ]; then
        printf 'null'
    else
        esc=$(printf '%s' "$val" | tr '\r\n\t' '   ' | sed 's/\\/\\\\/g; s/"/\\"/g')
        printf '"%s"' "$esc"
    fi
}

# Check live TUN interface
TUN_PRESENT="false"
if /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
    TUN_PRESENT="true"
fi

# Live RSS/HWM for codex-vpn-tunnel.service cgroup (or tunnel.pid)
RSS_KB=0
HWM_KB=0
CGRP="/sys/fs/cgroup/unified/system.slice/codex-vpn-tunnel.service/cgroup.procs"
if [ -r "$CGRP" ]; then
    for p in $(cat "$CGRP" 2>/dev/null || true); do
        r=$(awk '/^VmRSS:/{print $2}' "/proc/$p/status" 2>/dev/null || true)
        h=$(awk '/^VmHWM:/{print $2}' "/proc/$p/status" 2>/dev/null || true)
        RSS_KB=$((RSS_KB + ${r:-0}))
        HWM_KB=$((HWM_KB + ${h:-0}))
    done
elif [ -f "$RUN/tunnel.pid" ]; then
    tp=$(tr -dc '0-9' < "$RUN/tunnel.pid" 2>/dev/null || true)
    if [ -n "$tp" ] && [ -r "/proc/$tp/status" ]; then
        RSS_KB=$(awk '/^VmRSS:/{print $2}' "/proc/$tp/status" 2>/dev/null || echo 0)
        HWM_KB=$(awk '/^VmHWM:/{print $2}' "/proc/$tp/status" 2>/dev/null || echo 0)
    fi
fi

# Honest live state enforcement: never report connected if TUN is absent
TUNNEL_EST="false"
if [ "$STATUS" = "connected" ] && [ "$TUN_PRESENT" = "true" ] && [ -n "$ACTIVE_ID" ] && [ -n "$EXIT_IP" ]; then
    TUNNEL_EST="true"
elif [ "$STATUS" = "connected" ]; then
    STATUS="disconnected"
    AUTH_ID=""
    ACTIVE_ID=""
    EXIT_IP=""
    COLO=""
    ENDPOINT_IP=""
    TUNNEL_EST="false"
fi

DIVERGED="true"
if [ -n "$ACTIVE_ID" ] && [ "$SEL_ID" = "$ACTIVE_ID" ]; then
    DIVERGED="false"
fi

NOW_ISO=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '1970-01-01T00:00:00Z')
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -dc '0-9a-f-' || echo '')

SEL_JSON=$(loc_json "$SEL_ID")
AUTH_JSON=$(loc_json "$AUTH_ID")
ACT_JSON=$(loc_json "$ACTIVE_ID")

if [ -n "$ERR_STAGE" ] && [ -n "$ERR_CODE" ]; then
    rb="true"
    [ "$ERR_ROLLBACK" = "false" ] && rb="false"
    ERR_JSON=$(printf '{"stage":%s,"code":%s,"message":%s,"rollbackVerified":%s}' \
        "$(json_str_or_null "$ERR_STAGE")" \
        "$(json_str_or_null "$ERR_CODE")" \
        "$(json_str_or_null "$ERR_MSG")" \
        "$rb")
else
    ERR_JSON="null"
fi

# Build unique availableLocations array from endpoints.tsv
AVAIL_JSON="["
first=1
for lid in $(awk -F '\t' '{ if (!seen[$1]++) print $1 }' "$BASE/endpoints.tsv" 2>/dev/null || true); do
    item=$(loc_json "$lid")
    if [ "$item" != "null" ]; then
        if [ "$first" = "1" ]; then
            AVAIL_JSON="${AVAIL_JSON}${item}"
            first=0
        else
            AVAIL_JSON="${AVAIL_JSON},${item}"
        fi
    fi
done
AVAIL_JSON="${AVAIL_JSON}]"

TMP="$RUN/status.json.tmp.$$"
umask 022
cat > "$TMP" <<EOF
{
  "schemaVersion": 1,
  "observedAt": "${NOW_ISO}",
  "bootId": "${BOOT_ID}",
  "status": "${STATUS}",
  "selectedLocation": ${SEL_JSON},
  "authenticatedLocation": ${AUTH_JSON},
  "activeLocation": ${ACT_JSON},
  "tunnelEstablished": ${TUNNEL_EST},
  "selectedActiveDiverged": ${DIVERGED},
  "exitIp": $(json_str_or_null "$EXIT_IP"),
  "colo": $(json_str_or_null "$COLO"),
  "tunInterface": "dummylwtrial",
  "tunPresent": ${TUN_PRESENT},
  "endpointIp": $(json_str_or_null "$ENDPOINT_IP"),
  "rssKiB": ${RSS_KB:-0},
  "hwmKiB": ${HWM_KB:-0},
  "lastError": ${ERR_JSON},
  "availableLocations": ${AVAIL_JSON}
}
EOF
chmod 644 "$TMP" 2>/dev/null || true
if chsmack -a _ "$TMP" >/dev/null 2>&1; then
    mv -f "$TMP" "$RUN/status.json"
elif [ -w "$RUN/status.json" ]; then
    # Preserve pre-labeled Smack floor (_) on status.json when caller lacks CAP_MAC_ADMIN
    cat "$TMP" > "$RUN/status.json"
    rm -f "$TMP"
else
    mv -f "$TMP" "$RUN/status.json"
fi
