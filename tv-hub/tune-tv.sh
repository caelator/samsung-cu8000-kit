#!/bin/sh
# Tier 3 reversible whole-TV contention reclamation for Living Room Hub.
# Disable completely at any time by creating: /opt/usr/etc/codex-ui/no-tune
# Undo live at any time by sending SIGCONT to PIDs in /run/user/5001/codex-frozen-companions.pids

HUB_DIR="/opt/usr/etc/codex-ui"
PID_FILE="/run/user/5001/codex-frozen-companions.pids"

[ -f "$HUB_DIR/no-tune" ] && exit 0

# 1. Cut background zRAM recompaction thrash (measured bgrecompd 11.3% -> 1.25%)
sysctl -w vm.swappiness=40 >/dev/null 2>&1 || true
sysctl -w vm.vfs_cache_pressure=75 >/dev/null 2>&1 || true

# 2. Pause the 5 unused stock-Home companion daemons (auto-resumed via SIGCONT if Samsung Home opens)
# Deliberately excludes com.samsung.tv.csfs (Home fallback), iot-hub-control (boot hook), casting, and network.
COMPANIONS="DN_v.home.media DN_tv.searchall DN_home.sidebar DN_.multiscreen DN_v.mycontents"
mkdir -p /run/user/5001 2>/dev/null || true
: > "$PID_FILE"
chown 5001:users "$PID_FILE" 2>/dev/null || chmod 666 "$PID_FILE" 2>/dev/null || true

for name in $COMPANIONS; do
    for p in $(pidof "$name" 2>/dev/null); do
        [ -n "$p" ] || continue
        if [ -d /sys/fs/cgroup/cpu/background ]; then
            echo "$p" > /sys/fs/cgroup/cpu/background/tasks 2>/dev/null || true
        fi
        kill -STOP "$p" 2>/dev/null && echo "$p" >> "$PID_FILE"
    done
done
chmod 666 "$PID_FILE" 2>/dev/null || true
exit 0
