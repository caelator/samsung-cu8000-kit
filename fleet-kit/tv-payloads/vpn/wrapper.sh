#!/bin/sh
# Lightway direct-exec runner + policy-routing + live egress proof + 10s VmHWM watchdog.
# Generalized for any CU8000 property LAN (Wi-Fi or Ethernet, any RFC1918 subnet, optional local Plex).
# Runs inside codex-vpn-tunnel.service with ONLY CAP_NET_ADMIN (Smack System, NoNewPrivileges=yes).
# NEVER modifies main default route or /etc/resolv.conf; NEVER logs credentials.
set -eu

BASE="/opt/usr/etc/codex-vpn"
RUN="/run/codex-vpn"
CLIENT_BIN="$BASE/bin/lightway-client"
CLIENT_SHA="0d91d22f9270c0632bba5480fa187b70210939baece52aad1b3d848a6823dd01"
CA_FILE="$BASE/ca.pem"
CA_SHA="6d2e302a6092ef67c2bc61a7949851f0f99b540fb0a67e0367e91c8705bb10b0"
CRED_FILE="/opt/usr/codex-vpn-private/client.yaml"
LOG_FILE="$RUN/lightway.log"

write_state() {
    st="$1"; au="$2"; ac="$3"; ex="$4"; co="$5"; ep="$6"; es="$7"; ec="$8"; em="$9"
    umask 022
    cat > "$RUN/state.env.tmp.$$" <<EOF
STATUS='${st}'
AUTH_ID='${au}'
ACTIVE_ID='${ac}'
EXIT_IP='${ex}'
COLO='${co}'
ENDPOINT_IP='${ep}'
ERR_STAGE='${es}'
ERR_CODE='${ec}'
ERR_MSG='${em}'
ERR_ROLLBACK='true'
EOF
    mv -f "$RUN/state.env.tmp.$$" "$RUN/state.env"
    "$BASE/status-write.sh" || true
}

fail_and_exit() {
    stage="$1"; code="$2"; msg="$3"
    write_state "error" "" "" "" "" "" "$stage" "$code" "$msg"
    if [ -n "${CHILD_PID:-}" ] && kill -0 "$CHILD_PID" 2>/dev/null; then
        kill -TERM "$CHILD_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$CHILD_PID" 2>/dev/null || true
    fi
    exit 1
}

# Load session parameters prepared by ctl.sh
[ -f "$RUN/session.env" ] || fail_and_exit "handshake" "session_env_missing" "Pending VPN session config missing"
# shellcheck disable=SC1091
. "$RUN/session.env"

GATEWAY="${GATEWAY:-}"
IFACE="${IFACE:-}"
DEV_HOST="${DEV_HOST:-}"
PLEX_HOST="${PLEX_HOST:-}"
PLEX_PORT="${PLEX_PORT:-32400}"
LAN_CIDR="${LAN_CIDR:-}"
RESOLV_SHA="${RESOLV_SHA:-}"
ISP_IP="${ISP_IP:-}"
HWM_BUDGET_KB="${CODEX_VPN_HWM_BUDGET_KB:-98304}"
BYPASS_RFC1918="${BYPASS_RFC1918:-1}"

[ -n "$GATEWAY" ] && [ -n "$IFACE" ] && [ -n "$LAN_CIDR" ] \
    || fail_and_exit "routing" "network_baseline_missing" "Live LAN gateway/interface/subnet missing"

# Pin-verify binary and CA before exec
obs_client_sha=$(sha256sum "$CLIENT_BIN" 2>/dev/null | cut -d' ' -f1 || true)
[ "$obs_client_sha" = "$CLIENT_SHA" ] || fail_and_exit "handshake" "client_sha_mismatch" "Lightway client SHA-256 mismatch"
obs_ca_sha=$(sha256sum "$CA_FILE" 2>/dev/null | cut -d' ' -f1 || true)
[ "$obs_ca_sha" = "$CA_SHA" ] || fail_and_exit "handshake" "ca_sha_mismatch" "Lightway CA pin mismatch"
[ -r "$CRED_FILE" ] || fail_and_exit "handshake" "credentials_missing" "Private Lightway credentials unreadable"

# Refuse to start if dummylwtrial already exists
if /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
    fail_and_exit "routing" "tun_already_present" "Refusing to start while dummylwtrial exists"
fi

# Record active endpoint BEFORE launch so teardown always knows what host route to delete
printf '%s\n' "$ENDPOINT_IP" > "$RUN/active-endpoint.ip"
write_state "connecting" "" "" "" "" "$ENDPOINT_IP" "" "" ""

# Start Lightway directly (noexec for route & DNS; credentials only via mode-0600 file)
umask 077
: > "$LOG_FILE"
"$CLIENT_BIN" \
    --config-file "$CRED_FILE" \
    --route-mode noexec \
    --dns-config-mode noexec \
    --tun-name dummylwtrial \
    --server "${ENDPOINT_IP}:${ENDPOINT_PORT}" \
    --mode udp \
    --server-dn "$CERT_DN" \
    --ca-cert "$CA_FILE" \
    --outside-mtu "$MTU" \
    --tun-local-ip 100.64.0.6 \
    --tun-peer-ip 100.64.0.5 \
    --cipher aes256 \
    > "$LOG_FILE" 2>&1 &
CHILD_PID=$!
printf '%s\n' "$CHILD_PID" > "$RUN/tunnel.pid"

# Stage 1: Wait for dummylwtrial UP + Lightway Online event (up to 45s)
ready=0
i=0
while [ "$i" -lt 45 ]; do
    if ! kill -0 "$CHILD_PID" 2>/dev/null; then
        fail_and_exit "handshake" "client_exited_early" "Lightway client exited before reaching Online"
    fi
    if /sbin/ip link show dummylwtrial >/dev/null 2>&1 && grep -q 'Online' "$LOG_FILE" 2>/dev/null; then
        ready=1
        break
    fi
    sleep 1
    i=$((i + 1))
done
[ "$ready" = "1" ] || fail_and_exit "handshake" "handshake_timeout" "Lightway did not reach Online within 45s"

# Authenticated ONLY — NOT active yet!
write_state "connecting" "$LOC_ID" "" "" "" "$ENDPOINT_IP" "" "" ""

# Stage 2: Verify baseline main default before applying policy routing
def_before=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
case "$def_before" in
    "default via ${GATEWAY} dev ${IFACE}"*) ;;
    *) fail_and_exit "routing" "unexpected_main_default_before" "Main default route did not match LAN baseline" ;;
esac

# Apply isolated table 51820 + endpoint pin + explicit LAN/control/Plex/discovery bypasses
/sbin/ip route replace "${ENDPOINT_IP}/32" via "$GATEWAY" dev "$IFACE" table main \
    || fail_and_exit "routing" "endpoint_route_failed" "Failed to pin endpoint host route"
/sbin/ip route replace 100.64.0.5/32 dev dummylwtrial src 100.64.0.6 scope link table 51820 \
    || fail_and_exit "routing" "vpn_peer_route_failed" "Failed to add TUN peer route in table 51820"
/sbin/ip route replace 0.0.0.0/1 via 100.64.0.5 dev dummylwtrial table 51820 \
    || fail_and_exit "routing" "vpn_half0_route_failed" "Failed to add 0.0.0.0/1 in table 51820"
/sbin/ip route replace 128.0.0.0/1 via 100.64.0.5 dev dummylwtrial table 51820 \
    || fail_and_exit "routing" "vpn_half128_route_failed" "Failed to add 128.0.0.0/1 in table 51820"

# Delete any stale prefs first, then add protected exclusions BEFORE pref 500
for p in 100 110 120 130 140 150 160 170 180 190 500; do
    /sbin/ip rule del pref "$p" 2>/dev/null || true
done
if [ -n "$DEV_HOST" ]; then
    /sbin/ip rule add pref 100 to "${DEV_HOST}/32" lookup main || fail_and_exit "routing" "rule_100_failed" "Failed pref 100 rule"
fi
if [ -n "$PLEX_HOST" ]; then
    /sbin/ip rule add pref 110 to "${PLEX_HOST}/32" lookup main || fail_and_exit "routing" "rule_110_failed" "Failed pref 110 rule"
fi
/sbin/ip rule add pref 120 to "$LAN_CIDR" lookup main || fail_and_exit "routing" "rule_120_failed" "Failed pref 120 rule"
/sbin/ip rule add pref 130 to "${ENDPOINT_IP}/32" lookup main || fail_and_exit "routing" "rule_130_failed" "Failed pref 130 rule"
/sbin/ip rule add pref 140 to 224.0.0.0/4 lookup main || fail_and_exit "routing" "rule_140_failed" "Failed pref 140 rule"
/sbin/ip rule add pref 150 to 239.255.255.250/32 lookup main || fail_and_exit "routing" "rule_150_failed" "Failed pref 150 rule"
/sbin/ip rule add pref 160 to 255.255.255.255/32 lookup main || fail_and_exit "routing" "rule_160_failed" "Failed pref 160 rule"
if [ "$BYPASS_RFC1918" = "1" ]; then
    /sbin/ip rule add pref 170 to 10.0.0.0/8 lookup main || fail_and_exit "routing" "rule_170_failed" "Failed pref 170 rule"
    /sbin/ip rule add pref 180 to 172.16.0.0/12 lookup main || fail_and_exit "routing" "rule_180_failed" "Failed pref 180 rule"
    /sbin/ip rule add pref 190 to 192.168.0.0/16 lookup main || fail_and_exit "routing" "rule_190_failed" "Failed pref 190 rule"
fi
/sbin/ip rule add pref 500 to 0.0.0.0/0 lookup 51820 || fail_and_exit "routing" "rule_500_failed" "Failed pref 500 rule"

# Stage 3: Strict post-apply routing, DNS invariance, optional Plex LAN, and real exit IP verification
def_after=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
case "$def_after" in
    "default via ${GATEWAY} dev ${IFACE}"*) ;;
    *) fail_and_exit "routing" "main_default_regressed" "Main default route changed after apply" ;;
esac
case "$def_after" in
    *dummylwtrial*) fail_and_exit "routing" "main_default_points_to_tun" "Main default route mentions dummylwtrial" ;;
esac

rg_gw=$(/sbin/ip route get "$GATEWAY" 2>/dev/null | head -n 1 || true)
case "$rg_gw" in
    *"dev ${IFACE}"*) ;;
    *) fail_and_exit "control_check" "gateway_route_regressed" "LAN gateway route left primary interface" ;;
esac

if [ -n "$DEV_HOST" ]; then
    rg_dev=$(/sbin/ip route get "$DEV_HOST" 2>/dev/null | head -n 1 || true)
    case "$rg_dev" in
        *"dev ${IFACE}"*) ;;
        *) fail_and_exit "control_check" "dev_host_route_regressed" "Developer host route left primary interface" ;;
    esac
    case "$rg_dev" in
        *dummylwtrial*) fail_and_exit "control_check" "dev_host_routed_to_tun" "Developer host routed via TUN" ;;
    esac
fi

if [ -n "$PLEX_HOST" ]; then
    rg_plex=$(/sbin/ip route get "$PLEX_HOST" 2>/dev/null | head -n 1 || true)
    case "$rg_plex" in
        *"dev ${IFACE}"*) ;;
        *) fail_and_exit "control_check" "plex_route_regressed" "Plex host route left primary interface" ;;
    esac
    case "$rg_plex" in
        *dummylwtrial*) fail_and_exit "control_check" "plex_routed_to_tun" "Plex host routed via TUN" ;;
    esac
fi

rg_inet=$(/sbin/ip route get 1.1.1.1 2>/dev/null | head -n 1 || true)
case "$rg_inet" in
    *"dev dummylwtrial"*"table 51820"*|*"table 51820"*"dev dummylwtrial"*) ;;
    *) fail_and_exit "routing" "internet_not_routed_via_tun" "Public internet route not using dummylwtrial table 51820" ;;
esac

if [ -n "$RESOLV_SHA" ]; then
    cur_resolv=$(sha256sum /etc/resolv.conf 2>/dev/null | cut -d' ' -f1 || true)
    [ "$cur_resolv" = "$RESOLV_SHA" ] || fail_and_exit "control_check" "resolv_conf_changed" "/etc/resolv.conf hash changed"
fi

if [ -n "$PLEX_HOST" ]; then
    plex_code=$(/usr/bin/curl --silent --noproxy '*' --connect-timeout 3 --max-time 5 --output /dev/null --write-out '%{http_code}' "http://${PLEX_HOST}:${PLEX_PORT}/identity" 2>/dev/null || echo 000)
    [ "$plex_code" = "200" ] || fail_and_exit "control_check" "plex_lan_unreachable" "Plex LAN HTTP check returned ${plex_code}"
fi

# Prove real tunnel egress via 1.1.1.1/cdn-cgi/trace (must return global exit IP != ISP_IP + colo)
EXIT_IP=""
COLO=""
j=0
while [ "$j" -lt 8 ]; do
    if ! kill -0 "$CHILD_PID" 2>/dev/null; then
        fail_and_exit "activate_b" "client_died_before_egress" "Lightway exited before egress proof"
    fi
    trace=$(/usr/bin/curl --silent --noproxy '*' --connect-timeout 5 --max-time 10 https://1.1.1.1/cdn-cgi/trace 2>/dev/null || true)
    cand_ip=$(printf '%s\n' "$trace" | awk -F '=' '$1=="ip"{print $2; exit}' | tr -dc '0-9.')
    cand_colo=$(printf '%s\n' "$trace" | awk -F '=' '$1=="colo"{print $2; exit}' | tr -dc 'A-Za-z0-9')
    if [ -n "$cand_ip" ] && [ -n "$cand_colo" ]; then
        if [ -z "$ISP_IP" ] || [ "$cand_ip" != "$ISP_IP" ]; then
            EXIT_IP="$cand_ip"
            COLO="$cand_colo"
            break
        fi
    fi
    sleep 3
    j=$((j + 1))
done
[ -n "$EXIT_IP" ] || fail_and_exit "activate_b" "egress_proof_failed" "Failed to prove distinct global VPN exit IP"

# ALL checks passed -> mark active & connected!
write_state "connected" "$LOC_ID" "$LOC_ID" "$EXIT_IP" "$COLO" "$ENDPOINT_IP" "" "" ""

# Stage 4: Steady-state 10s watchdog (VmHWM <= 96 MiB + default route & TUN guard)
tick=0
CGRP="/sys/fs/cgroup/unified/system.slice/codex-vpn-tunnel.service/cgroup.procs"
while kill -0 "$CHILD_PID" 2>/dev/null; do
    sleep 10
    kill -0 "$CHILD_PID" 2>/dev/null || break

    if ! /sbin/ip link show dummylwtrial >/dev/null 2>&1; then
        fail_and_exit "control_check" "tun_disappeared" "dummylwtrial vanished while running"
    fi
    cur_def=$(/sbin/ip -4 route show default 2>/dev/null | head -n 1 || true)
    case "$cur_def" in
        "default via ${GATEWAY} dev ${IFACE}"*) ;;
        *) fail_and_exit "control_check" "default_route_drifted" "Main default route drifted during session" ;;
    esac

    total_hwm=0
    if [ -r "$CGRP" ]; then
        for pid in $(cat "$CGRP" 2>/dev/null || true); do
            [ "$pid" = "$$" ] && continue
            hwm=$(awk '/^VmHWM:/{print $2}' "/proc/$pid/status" 2>/dev/null || true)
            total_hwm=$((total_hwm + ${hwm:-0}))
        done
    else
        hwm=$(awk '/^VmHWM:/{print $2}' "/proc/$CHILD_PID/status" 2>/dev/null || true)
        total_hwm=${hwm:-0}
    fi
    if [ "$total_hwm" -gt "$HWM_BUDGET_KB" ]; then
        fail_and_exit "supervision" "hwm_over_budget" "Summed VmHWM ${total_hwm} KiB exceeded ${HWM_BUDGET_KB} KiB budget"
    fi

    tick=$((tick + 1))
    if [ $((tick % 3)) -eq 0 ]; then
        "$BASE/status-write.sh" 2>/dev/null || true
    fi
done

rc=0
wait "$CHILD_PID" || rc=$?
if [ "$rc" -ne 0 ]; then
    write_state "error" "" "" "" "" "" "supervision" "client_exited_nonzero" "Lightway client exited with code ${rc}"
fi
exit "$rc"
