#!/bin/sh
# Root-owned Lightway VPN lifecycle controller for Samsung CU8000 (Fleet Kit).
# Auto-detects live LAN gateway/interface/subnet across reboots & any property network.
# Invoked by codex-vpn-ctld (strict allowlist) or root host operator.
# Enforces strict separation: selectedLocation vs authenticatedLocation vs activeLocation.
set -eu

BASE="/opt/usr/etc/codex-vpn"
RUN="/run/codex-vpn"
UNIT="codex-vpn-tunnel.service"
DEADMAN_BASE="codex-vpn-deadman"
LOCK_DIR="$RUN/op.lock"
DEFAULT_DEADMAN_SEC="${CODEX_VPN_DEADMAN_SEC:-28800}"

mkdir -p "$RUN"
chmod 755 "$RUN" 2>/dev/null || true

acquire_lock() {
    i=0
    while ! mkdir "$LOCK_DIR" 2>/dev/null; do
        if [ -f "$LOCK_DIR/pid" ]; then
            oldpid=$(tr -dc '0-9' < "$LOCK_DIR/pid" 2>/dev/null || true)
            if [ -n "$oldpid" ] && ! kill -0 "$oldpid" 2>/dev/null; then
                rm -rf "$LOCK_DIR" 2>/dev/null || true
                continue
            fi
        fi
        i=$((i + 1))
        if [ "$i" -ge 30 ]; then
            printf '{"ok":false,"error":"operation_in_progress"}\n'
            exit 1
        fi
        sleep 1
    done
    printf '%s\n' "$$" > "$LOCK_DIR/pid"
    trap 'rm -rf "$LOCK_DIR" 2>/dev/null || true' EXIT INT TERM
}

detect_network_baseline() {
    GATEWAY=""
    IFACE=""
    DEV_HOST=""
    PLEX_HOST=""
    PLEX_PORT="32400"
    LAN_CIDR=""
    BYPASS_RFC1918="1"
    ISP_IP=""

    if [ -f "$BASE/network.env" ]; then
        # shellcheck disable=SC1091
        . "$BASE/network.env" 2>/dev/null || true
    fi
    if [ -f "$RUN/baseline.env" ]; then
        # shellcheck disable=SC1091
        . "$RUN/baseline.env" 2>/dev/null || true
    fi

    # Always derive live gateway / interface / LAN subnet from the TV's actual routing table
    def=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
    live_gw=$(printf '%s\n' "$def" | awk '{for(i=1;i<=NF;i++) if($i=="via") {print $(i+1); exit}}')
    live_if=$(printf '%s\n' "$def" | awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}')
    [ -n "$live_gw" ] && GATEWAY="$live_gw"
    [ -n "$live_if" ] && IFACE="$live_if"

    if [ -n "$IFACE" ]; then
        live_cidr=$(/sbin/ip -4 route show dev "$IFACE" proto kernel scope link 2>/dev/null | awk '{print $1; exit}')
        [ -n "$live_cidr" ] && LAN_CIDR="$live_cidr"
    fi
    if [ -z "$LAN_CIDR" ] && [ -n "$GATEWAY" ]; then
        LAN_CIDR="$(printf '%s' "$GATEWAY" | awk -F'.' '{print $1"."$2"."$3".0/24"}')"
    fi

    RESOLV_SHA=$(sha256sum /etc/resolv.conf 2>/dev/null | cut -d' ' -f1 || true)
}

read_selected_id() {
    if [ -f "$BASE/selected-location.id" ]; then
        val=$(tr -dc '0-9' < "$BASE/selected-location.id" 2>/dev/null | head -c 6 || true)
        [ -n "$val" ] && { printf '%s' "$val"; return; }
    fi
    first_id=$(awk -F '\t' 'NF>=7 {print $1; exit}' "$BASE/endpoints.tsv" 2>/dev/null || true)
    printf '%s' "${first_id:-54}"
}

lookup_endpoint_row() {
    lid="$1"
    idx="${2:-1}"
    awk -F '\t' -v id="$lid" -v n="$idx" '$1 == id { c++; if (c == n) { print; exit } }' "$BASE/endpoints.tsv" 2>/dev/null || true
}

count_endpoint_rows() {
    lid="$1"
    awk -F '\t' -v id="$lid" '$1 == id { c++ } END { print c+0 }' "$BASE/endpoints.tsv" 2>/dev/null || echo 0
}

arm_deadman() {
    secs="$1"
    case "$secs" in
        *[!0-9]*|"") secs=300 ;;
    esac
    [ "$secs" -lt 15 ] && secs=15
    systemctl stop "${DEADMAN_BASE}.timer" "${DEADMAN_BASE}.service" >/dev/null 2>&1 || true
    systemctl reset-failed "${DEADMAN_BASE}.timer" "${DEADMAN_BASE}.service" >/dev/null 2>&1 || true
    /usr/bin/systemd-run --no-ask-password --service-type=exec \
        --unit="${DEADMAN_BASE}.service" \
        --on-active="${secs}s" \
        --timer-property=AccuracySec=1ms \
        --timer-property=RandomizedDelaySec=0 \
        --timer-property=RemainAfterElapse=no \
        -p User=0 -p Group=0 -p SmackProcessLabel=System \
        -p NoNewPrivileges=yes \
        -p CapabilityBoundingSet=CAP_SYS_ADMIN \
        -p AmbientCapabilities=CAP_SYS_ADMIN \
        -p KillMode=control-group -p SendSIGKILL=yes -p Restart=no \
        -p RuntimeMaxSec=5 -p TimeoutStartSec=2 -p TimeoutStopSec=2 \
        -p LimitCORE=0 -p LimitNOFILE=32 -p TasksMax=8 \
        -p StandardInput=null \
        /usr/bin/systemctl --no-ask-password stop "$UNIT" >/dev/null 2>&1
}

disarm_deadman() {
    systemctl stop "${DEADMAN_BASE}.timer" "${DEADMAN_BASE}.service" >/dev/null 2>&1 || true
    systemctl reset-failed "${DEADMAN_BASE}.timer" "${DEADMAN_BASE}.service" >/dev/null 2>&1 || true
}

emit_reply() {
    ok_flag="$1"
    action="$2"
    "$BASE/status-write.sh" 2>/dev/null || true
    st_json=$(tr '\n' ' ' < "$RUN/status.json" 2>/dev/null || echo '{}')
    printf '{"ok":%s,"action":"%s","vpn":%s}\n' "$ok_flag" "$action" "$st_json"
}

do_status() {
    "$BASE/status-write.sh" 2>/dev/null || true
    emit_reply "true" "status"
}

do_select() {
    lid="$1"
    row=$(lookup_endpoint_row "$lid")
    if [ -z "$row" ]; then
        printf '{"ok":false,"action":"select","error":"unknown_location_id"}\n'
        exit 1
    fi
    umask 022
    printf '%s\n' "$lid" > "$BASE/selected-location.id"
    chmod 644 "$BASE/selected-location.id" 2>/dev/null || true
    emit_reply "true" "select"
}

do_select_next() {
    cur=$(read_selected_id)
    next=$(awk -F '\t' -v cur="$cur" '
        NF >= 7 && !seen[$1]++ { ids[++n] = $1 }
        END {
            if (n == 0) { print cur; exit }
            for (i = 1; i <= n; i++) {
                if (ids[i] == cur) { print ids[(i % n) + 1]; exit }
            }
            print ids[1]
        }
    ' "$BASE/endpoints.tsv" 2>/dev/null || echo 54)
    do_select "$next"
}

do_disconnect_inner() {
    systemctl stop "$UNIT" >/dev/null 2>&1 || true
    systemctl reset-failed "$UNIT" >/dev/null 2>&1 || true
    disarm_deadman
    "$BASE/teardown.sh" >/dev/null 2>&1 || true

    if ! /sbin/ip link show dummylwtrial >/dev/null 2>&1 && \
       [ "$(/sbin/ip route show table 51820 2>/dev/null | wc -l | tr -dc '0-9')" = "0" ]; then
        umask 022
        cat > "$RUN/state.env.tmp.$$" <<EOF
STATUS='disconnected'
AUTH_ID=''
ACTIVE_ID=''
EXIT_IP=''
COLO=''
ENDPOINT_IP=''
ERR_STAGE=''
ERR_CODE=''
ERR_MSG=''
ERR_ROLLBACK='true'
EOF
        mv -f "$RUN/state.env.tmp.$$" "$RUN/state.env"
        "$BASE/status-write.sh" 2>/dev/null || true
        return 0
    fi
    return 1
}

verify_clean_control_routes() {
    detect_network_baseline
    [ -n "$GATEWAY" ] && [ -n "$IFACE" ] && [ -n "$LAN_CIDR" ] || return 1

    /sbin/ip link show dummylwtrial >/dev/null 2>&1 && return 1
    [ "$(/sbin/ip route show table 51820 2>/dev/null | wc -l | tr -dc '0-9')" = "0" ] || return 1
    /sbin/ip rule list 2>/dev/null | grep -q 'lookup 51820' && return 1
    def=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
    case "$def" in
        "default via ${GATEWAY} dev ${IFACE}"*) ;;
        *) return 1 ;;
    esac
    if [ -n "$DEV_HOST" ]; then
        rg_dev=$(/sbin/ip route get "$DEV_HOST" 2>/dev/null | head -n 1 || true)
        case "$rg_dev" in
            *"dev ${IFACE}"*) ;;
            *) return 1 ;;
        esac
    fi
    if [ -n "$PLEX_HOST" ]; then
        rg_plex=$(/sbin/ip route get "$PLEX_HOST" 2>/dev/null | head -n 1 || true)
        case "$rg_plex" in
            *"dev ${IFACE}"*) ;;
            *) return 1 ;;
        esac
        pcode=$(/usr/bin/curl --silent --noproxy '*' --connect-timeout 3 --max-time 5 --output /dev/null --write-out '%{http_code}' "http://${PLEX_HOST}:${PLEX_PORT}/identity" 2>/dev/null || echo 000)
        [ "$pcode" = "200" ] || return 1
    fi
    return 0
}

do_connect_inner() {
    lid="$1"
    deadman_sec="${2:-$DEFAULT_DEADMAN_SEC}"
    total_eps=$(count_endpoint_rows "$lid")
    if [ "$total_eps" -lt 1 ]; then
        printf '{"ok":false,"action":"connect","error":"unknown_location_id"}\n'
        return 1
    fi

    umask 022
    printf '%s\n' "$lid" > "$BASE/selected-location.id"
    chmod 644 "$BASE/selected-location.id" 2>/dev/null || true

    ep_idx=1
    while [ "$ep_idx" -le "$total_eps" ]; do
        row=$(lookup_endpoint_row "$lid" "$ep_idx")
        [ -n "$row" ] || break

        if systemctl is-active --quiet "$UNIT" 2>/dev/null || /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
            do_disconnect_inner || true
        fi
        if ! verify_clean_control_routes; then
            cat > "$RUN/state.env" <<EOF
STATUS='error'
AUTH_ID=''
ACTIVE_ID=''
EXIT_IP=''
COLO=''
ENDPOINT_IP=''
ERR_STAGE='verify_control_routes'
ERR_CODE='pre_connect_control_routes_invalid'
ERR_MSG='Protected LAN/control routes did not verify before connecting'
ERR_ROLLBACK='true'
EOF
            "$BASE/status-write.sh" 2>/dev/null || true
            return 1
        fi

        # Capture current ISP IP before opening tunnel if not already known
        if [ -z "$ISP_IP" ]; then
            tr_pre=$(/usr/bin/curl --silent --noproxy '*' --connect-timeout 4 --max-time 6 https://1.1.1.1/cdn-cgi/trace 2>/dev/null || true)
            ISP_IP=$(printf '%s\n' "$tr_pre" | awk -F '=' '$1=="ip"{print $2; exit}' | tr -dc '0-9.')
        fi

        # Refresh /run/codex-vpn/baseline.env so teardown always sees live gateway/iface
        umask 022
        cat > "$RUN/baseline.env" <<EOF
GATEWAY='${GATEWAY}'
IFACE='${IFACE}'
DEV_HOST='${DEV_HOST}'
PLEX_HOST='${PLEX_HOST}'
PLEX_PORT='${PLEX_PORT}'
LAN_CIDR='${LAN_CIDR}'
RESOLV_SHA='${RESOLV_SHA}'
ISP_IP='${ISP_IP}'
BYPASS_RFC1918='${BYPASS_RFC1918}'
EOF

        loc_name=$(printf '%s' "$row" | cut -f2)
        loc_cc=$(printf '%s' "$row" | cut -f3)
        ep_ip=$(printf '%s' "$row" | cut -f4)
        ep_port=$(printf '%s' "$row" | cut -f5)
        ep_dn=$(printf '%s' "$row" | cut -f6)
        ep_mtu=$(printf '%s' "$row" | cut -f7)

        cat > "$RUN/session.env" <<EOF
LOC_ID='${lid}'
LOC_NAME='${loc_name}'
LOC_CC='${loc_cc}'
ENDPOINT_IP='${ep_ip}'
ENDPOINT_PORT='${ep_port}'
CERT_DN='${ep_dn}'
MTU='${ep_mtu}'
GATEWAY='${GATEWAY}'
IFACE='${IFACE}'
DEV_HOST='${DEV_HOST}'
PLEX_HOST='${PLEX_HOST}'
PLEX_PORT='${PLEX_PORT}'
LAN_CIDR='${LAN_CIDR}'
RESOLV_SHA='${RESOLV_SHA}'
ISP_IP='${ISP_IP}'
BYPASS_RFC1918='${BYPASS_RFC1918}'
EOF

        # Pre-arm short 180s connect-guard TV-local dead-man timer BEFORE starting tunnel!
        arm_deadman 180 || return 1

        systemctl reset-failed "$UNIT" >/dev/null 2>&1 || true
        /usr/bin/systemd-run --no-ask-password --service-type=exec \
            --unit="$UNIT" \
            --description="CU8000 Lightway VPN Tunnel (transient)" \
            -p User=0 -p Group=0 -p SmackProcessLabel=System \
            -p NoNewPrivileges=yes \
            -p CapabilityBoundingSet=CAP_NET_ADMIN \
            -p AmbientCapabilities=CAP_NET_ADMIN \
            -p KillMode=control-group -p SendSIGKILL=yes \
            -p Restart=no \
            -p TimeoutStartSec=30 -p TimeoutStopSec=10 \
            -p RemainAfterExit=no -p Delegate=no \
            -p LimitCORE=0 -p LimitNOFILE=128 -p UMask=0077 \
            -p TasksMax=64 -p OOMScoreAdjust=500 -p MemoryMax=100663296 \
            -p WorkingDirectory="$BASE" \
            -p StandardInput=null \
            -p StandardOutput=file:"$RUN/wrapper.log" \
            -p StandardError=file:"$RUN/wrapper.log" \
            -p "ExecStopPost=$BASE/teardown.sh" \
            "$BASE/wrapper.sh" >/dev/null 2>&1

        w=0
        err_stage=""
        while [ "$w" -lt 75 ]; do
            sleep 1
            cur_st=""
            if [ -f "$RUN/state.env" ]; then
                cur_st=$(awk -F "'" '$1=="STATUS="{print $2; exit}' "$RUN/state.env" 2>/dev/null || true)
                err_stage=$(awk -F "'" '$1=="ERR_STAGE="{print $2; exit}' "$RUN/state.env" 2>/dev/null || true)
            fi
            if [ "$cur_st" = "connected" ]; then
                arm_deadman "$deadman_sec" || true
                return 0
            fi
            if [ "$cur_st" = "error" ] || ! systemctl is-active --quiet "$UNIT" 2>/dev/null; then
                break
            fi
            w=$((w + 1))
        done

        systemctl stop "$UNIT" >/dev/null 2>&1 || true
        disarm_deadman
        "$BASE/teardown.sh" >/dev/null 2>&1 || true

        if [ "$err_stage" = "handshake" ] && [ "$ep_idx" -lt "$total_eps" ]; then
            ep_idx=$((ep_idx + 1))
            continue
        fi
        return 1
    done
    return 1
}

cmd="${1:-status}"
arg="${2:-}"
arg2="${3:-}"

case "$cmd" in
    status)
        do_status
        ;;
    select)
        acquire_lock
        do_select "$arg"
        ;;
    select-next)
        acquire_lock
        do_select_next
        ;;
    disconnect)
        acquire_lock
        if do_disconnect_inner; then
            emit_reply "true" "disconnect"
        else
            emit_reply "false" "disconnect"
            exit 1
        fi
        ;;
    connect)
        acquire_lock
        target="${arg:-$(read_selected_id)}"
        if do_connect_inner "$target" "${arg2:-$DEFAULT_DEADMAN_SEC}"; then
            emit_reply "true" "connect"
        else
            emit_reply "false" "connect"
            exit 1
        fi
        ;;
    switch)
        acquire_lock
        target="$arg"
        if [ -z "$target" ] || [ -z "$(lookup_endpoint_row "$target")" ]; then
            printf '{"ok":false,"action":"switch","error":"unknown_location_id"}\n'
            exit 1
        fi
        do_disconnect_inner || { emit_reply "false" "switch"; exit 1; }
        if ! verify_clean_control_routes; then
            emit_reply "false" "switch"
            exit 1
        fi
        if do_connect_inner "$target" "${arg2:-$DEFAULT_DEADMAN_SEC}"; then
            emit_reply "true" "switch"
        else
            emit_reply "false" "switch"
            exit 1
        fi
        ;;
    toggle)
        acquire_lock
        if systemctl is-active --quiet "$UNIT" 2>/dev/null && /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
            if do_disconnect_inner; then
                emit_reply "true" "toggle"
            else
                emit_reply "false" "toggle"
                exit 1
            fi
        else
            target=$(read_selected_id)
            if do_connect_inner "$target" "$DEFAULT_DEADMAN_SEC"; then
                emit_reply "true" "toggle"
            else
                emit_reply "false" "toggle"
                exit 1
            fi
        fi
        ;;
    arm-deadman)
        acquire_lock
        arm_deadman "${arg:-$DEFAULT_DEADMAN_SEC}"
        emit_reply "true" "arm-deadman"
        ;;
    disarm-deadman)
        acquire_lock
        disarm_deadman
        emit_reply "true" "disarm-deadman"
        ;;
    *)
        printf '{"ok":false,"error":"unknown_command"}\n'
        exit 64
        ;;
esac
