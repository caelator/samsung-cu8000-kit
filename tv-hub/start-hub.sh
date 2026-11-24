#!/bin/sh
# Launch Living Room Hub as a clean transient systemd service under uid 5001 (Wayland user).
# Prefers Tier 4 bare-metal ARMv7 C GLES2+ASTC engine; automatically falls back to .NET NUI build.
# Rollback / stop at any time: systemctl stop codex-livingroom-hub

HUB_DIR="/opt/usr/etc/codex-ui"
UNIT="codex-livingroom-hub"
LOG="/run/user/5001/living-room-hub.log"
NATIVE_BIN="$HUB_DIR/living-room-hub-native"
DOTNET_DLL="$HUB_DIR/LivingRoomHub.dll"

[ -x "$NATIVE_BIN" ] || [ -f "$DOTNET_DLL" ] || exit 0
mkdir -p "$HUB_DIR/actions" 2>/dev/null || true

# Wait for Wayland compositor socket (up to 60s on boot)
i=0
while [ ! -S /run/user/5001/wayland-0 ] && [ "$i" -lt 60 ]; do
    sleep 1
    i=$((i + 1))
done
[ -S /run/user/5001/wayland-0 ] || exit 1

# Apply Tier 3 reversible VM + companion tuning after boot settles (non-blocking)
if [ -x "$HUB_DIR/tune-tv.sh" ]; then
    (sleep 5; "$HUB_DIR/tune-tv.sh") >/dev/null 2>&1 &
fi

if systemctl is-active --quiet "$UNIT"; then
    exit 0
fi

systemctl reset-failed "$UNIT" 2>/dev/null || true

# Create runner wrapper that tries bare-metal native binary first, falling back to .NET if exec fails (e.g. UEP active)
RUNNER="$HUB_DIR/run-engine.sh"
cat > "$RUNNER" <<'EOF'
#!/bin/sh
HUB_DIR="/opt/usr/etc/codex-ui"
LOG="/run/user/5001/living-room-hub.log"
if [ ! -f "$HUB_DIR/force-dotnet" ] && [ -x "$HUB_DIR/living-room-hub-native" ]; then
    "$HUB_DIR/living-room-hub-native" "$HUB_DIR" >>"$LOG" 2>&1
    rc=$?
    [ "$rc" -eq 0 ] && exit 0
    echo "[start-hub] native engine exited rc=$rc; falling back to .NET NUI" >>"$LOG"
fi
exec /usr/bin/dotnet "$HUB_DIR/LivingRoomHub.dll" >>"$LOG" 2>&1
EOF
chmod 755 "$RUNNER"

exec /usr/bin/systemd-run \
    --unit="$UNIT" \
    --description="Living Room Hub TV Interface" \
    --property=User=5001 \
    --property=Group=users \
    --property="WorkingDirectory=$HUB_DIR" \
    --property=Nice=-5 \
    --property=OOMScoreAdjust=-300 \
    --property=Restart=on-failure \
    --property=RestartSec=3 \
    --setenv=WAYLAND_DISPLAY=wayland-0 \
    --setenv=XDG_RUNTIME_DIR=/run/user/5001 \
    --setenv=ELM_DISPLAY=wl \
    --setenv=ELM_ACCEL=gl \
    --setenv=ELM_PROFILE=tv \
    --setenv=TIZEN_API_VERSION=6.0 \
    --setenv="DBUS_SESSION_BUS_ADDRESS=kernel:path=/sys/fs/kdbus/5001-user/bus;unix:path=/run/user/5001/dbus/user_bus_socket" \
    --setenv=DOTNET_gcServer=0 \
    --setenv=DOTNET_TieredCompilation=0 \
    --setenv=DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
    --setenv=DALI_REFRESH_RATE=1 \
    --setenv=DALI_DISABLE_PARTIAL_UPDATE=0 \
    --setenv=DALI_MULTI_SAMPLING_LEVEL=0 \
    --setenv=DALI_DISABLE_DEPTH_BUFFER=1 \
    --setenv=DALI_DISABLE_STENCIL_BUFFER=1 \
    --setenv=DALI_ENABLE_CACHE_RENDERED_GLYPH=1 \
    --setenv=DALI_PRECOMPILE_SHADER=1 \
    --setenv=DALI_ASYNC_MANAGER_THREAD_POOL_SIZE=2 \
    --setenv=DALI_ASYNC_MANAGER_LOW_PRIORITY_THREAD_POOL_SIZE=1 \
    --setenv=DALI_DISABLE_ATSPI=1 \
    --setenv=DALI_SUPPRESS_SCREEN_READER=1 \
    --setenv=DALI_MATCH_SYSTEM_LANGUAGE_DIRECTION=0 \
    --setenv=ECORE_VSYNC_DRM_ALL=1 \
    --setenv=ECORE_WL_INPUT_KEY_CONVERSION_DISABLE=1 \
    "$RUNNER"
