#!/usr/bin/awk -f
# Installed by CU8000 Fleet Kit. Runs the on-boot setup unit asynchronously so
# Samsung's iot-hub-control-setup boot service is never delayed, then executes
# Samsung's original untouched hub-init.sh.samsung exactly as shipped.
BEGIN {
    arg = ARGV[1]; ARGC = 1
    system("/usr/bin/systemd-run --quiet --no-block --collect --unit=codex-onboot /bin/sh -c \". /opt/usr/etc/codex/onboot.sh\" >/dev/null 2>&1")
    status = system("/opt/usr/apps/com.samsung.tv.iot-hub-control/bin/hub-init.sh.samsung " arg)
    exit int(status / 256)
}
