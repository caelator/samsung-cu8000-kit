#!/bin/sh
# Idempotent, fail-closed TV-local teardown for Lightway VPN (CU8000 Fleet Kit).
# Invoked by ExecStopPost on codex-vpn-tunnel.service and directly by ctl.sh.
# NEVER touches the main default route, /etc/resolv.conf, iptables, or protected Samsung units.
set +e

BASE="/opt/usr/etc/codex-vpn"
RUN="/run/codex-vpn"

GATEWAY=""
IFACE=""
if [ -f "$BASE/network.env" ]; then
    # shellcheck disable=SC1091
    . "$BASE/network.env" 2>/dev/null
fi
if [ -f "$RUN/baseline.env" ]; then
    # shellcheck disable=SC1091
    . "$RUN/baseline.env" 2>/dev/null
fi

# Auto-detect live gateway/interface if missing
if [ -z "$GATEWAY" ] || [ -z "$IFACE" ]; then
    def=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
    GATEWAY="${GATEWAY:-$(printf '%s\n' "$def" | awk '{for(i=1;i<=NF;i++) if($i=="via") {print $(i+1); exit}}')}"
    IFACE="${IFACE:-$(printf '%s\n' "$def" | awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}')}"
fi

# 1. Remove all Lightway policy rules (looped to clear any duplicates)
for pref in 100 110 120 130 140 150 160 170 180 190 500; do
    i=0
    while [ "$i" -lt 4 ]; do
        /sbin/ip rule del pref "$pref" 2>/dev/null || break
        i=$((i + 1))
    done
done

# 2. Flush isolated VPN routing table 51820
/sbin/ip route flush table 51820 2>/dev/null || true

# 3. Remove pinned provider endpoint host route(s) from table main
if [ -f "$RUN/active-endpoint.ip" ]; then
    while IFS= read -r ep; do
        case "$ep" in
            *[!0-9.]*|"") continue ;;
            *.*.*.*)
                if [ -n "$GATEWAY" ] && [ -n "$IFACE" ]; then
                    /sbin/ip route del "${ep}/32" via "$GATEWAY" dev "$IFACE" table main 2>/dev/null || true
                else
                    /sbin/ip route del "${ep}/32" table main 2>/dev/null || true
                fi
                ;;
        esac
    done < "$RUN/active-endpoint.ip"
    rm -f "$RUN/active-endpoint.ip" 2>/dev/null || true
fi

# 4. Remove app-owned TUN interface only
/sbin/ip link set dev dummylwtrial down 2>/dev/null || true
/sbin/ip link del dev dummylwtrial 2>/dev/null || true
rm -f "$RUN/tunnel.pid" 2>/dev/null || true

# 5. Update public state preserving selectedLocation and any recorded error
PREV_STATUS="disconnected"
ERR_STAGE=""
ERR_CODE=""
ERR_MSG=""
if [ -f "$RUN/state.env" ]; then
    # shellcheck disable=SC1091
    . "$RUN/state.env" 2>/dev/null
    PREV_STATUS="${STATUS:-disconnected}"
fi

NEXT_STATUS="disconnected"
if [ "$PREV_STATUS" = "error" ] && [ -n "$ERR_CODE" ]; then
    NEXT_STATUS="error"
fi

RB_OK="true"
if /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
    RB_OK="false"
fi
if [ "$(/sbin/ip route show table 51820 2>/dev/null | wc -l | tr -dc '0-9')" != "0" ]; then
    RB_OK="false"
fi

umask 022
cat > "$RUN/state.env.tmp.$$" <<EOF
STATUS='${NEXT_STATUS}'
AUTH_ID=''
ACTIVE_ID=''
EXIT_IP=''
COLO=''
ENDPOINT_IP=''
ERR_STAGE='${ERR_STAGE}'
ERR_CODE='${ERR_CODE}'
ERR_MSG='${ERR_MSG}'
ERR_ROLLBACK='${RB_OK}'
EOF
mv -f "$RUN/state.env.tmp.$$" "$RUN/state.env" 2>/dev/null || true
"$BASE/status-write.sh" 2>/dev/null || true
exit 0
