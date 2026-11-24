#!/bin/sh
# On-device start-up setup for Samsung CU8000 (CU8000 Fleet Kit).
# Started at every boot by iot-hub-control-setup via hub-init.sh (awk wrapper).
# Everything here lives in RAM or writable /opt: zero boot-area or system-file writes.
D=/opt/usr/etc/codex
U="swu-auto-upgrade.service swu-auto-upgrade.timer swu-auto-upgrade.path swu-verifier-tv.service swu-verifier-tv.timer swu-core-tv.service swu-service.service vdsystem-update-manager.service vdsystem-update-manager.timer fw-update-service.service fw-update-service.path fw-update-init-download.service was-background-update.service was-background-update.path"
S="adagent-service.service pisa-service.path pisa-service.socket pisa-service.service canalysis-service.service com.samsung.tv.context-aware-agent.path com.samsung.tv.context-aware-agent.service com.samsung.tv.context-aware-service.path com.samsung.tv.context-aware-service.service ipa.service abnormal_user_behavior.service org.tizen.tv.sp-kpi.service org.tizen.tv.tagpro-service-tv.service mirroring-recognition-manager.service contents-recognition-service.service slive-provider-daemon.path slive-provider-daemon.service bms.path bms.service esg-service.service org.tizen.voice-app.path org.tizen.voice-app.service org.tizen.voice-client.path org.tizen.voice-client.service com.samsung.tv.bixby-prov.path com.samsung.tv.bixby-prov.service org.tizen.multi-assistant-service.path org.tizen.multi-assistant-service.service com.samsung.tv.ondevice-voice.path com.samsung.tv.ondevice-voice.service tts-download-service.path tts-download-service.service com.samsung.tv.ghdaemon.path com.samsung.tv.ghdaemon.service gamepad-service.service gamepad-driver.service org.tizen.remotepc.service mde-framework.service"

# Defaults match verified CU8000 build 0090; per-TV boot.conf overrides for 0080 or custom profiles.
UEP_KO="uepclear5.ko"
UEP_KO_SHA="0a69a4add6e12a75b4e20156cc6e6b4537488304aaa2759b82ed7b8bf4564c16"
UEP_LOG_TAG="codex-uep5"
LIB_STOCK_SHA="3484503c53df7bba4c664812ae985eca363faecde97dd36e456beaecc3689457"
LIB_PATCHED_SHA="dcad0d175a8010dbb0f41a33e66db44baccb461e522d23c9f45d1d5bb78bec71"
STRIP_ENABLED="1"
HUB_ENABLED="1"
VPN_CTLD_ENABLED="1"
VPN_HUB_SMACK="System::Privileged"
VPN_USERS_GID="100"

if [ -f "$D/boot.conf" ]; then
    # shellcheck disable=SC1091
    . "$D/boot.conf" 2>/dev/null || true
fi

{
    echo "=== $(date '+%Y-%m-%d %H:%M:%S') boot=$(cat /proc/sys/kernel/random/boot_id)"

    # 1. Anchor-gated kernel UEP relaxation in RAM (fail-closed if module/hash absent or anchors differ)
    if [ -n "$UEP_KO" ] && [ -n "$UEP_KO_SHA" ] && [ -f "$D/$UEP_KO" ]; then
        if [ "$(sha256sum "$D/$UEP_KO" | cut -d' ' -f1)" = "$UEP_KO_SHA" ]; then
            /usr/sbin/insmod "$D/$UEP_KO" 2>&1
            dmesg | grep -E 'codex-uep5|uepclear6' | tail -1
        else
            echo "module hash mismatch ($UEP_KO): not loading"
        fi
    else
        echo "no kernel UEP module configured for this firmware build; skipping insmod"
    fi

    # 2. Shell script-signature check relaxation via RAM-only bind mount over RO /usr/lib/libkUEPUser.so
    if [ -n "$LIB_STOCK_SHA" ] && [ -n "$LIB_PATCHED_SHA" ] && [ -f "$D/libkUEPUser.relaxed.so" ]; then
        if [ "$(sha256sum /usr/lib/libkUEPUser.so | cut -d' ' -f1)" = "$LIB_STOCK_SHA" ] && \
           [ "$(sha256sum "$D/libkUEPUser.relaxed.so" | cut -d' ' -f1)" = "$LIB_PATCHED_SHA" ]; then
            if grep -q ' /usr/lib/libkUEPUser.so ' /proc/mounts; then
                echo "script check already relaxed"
            elif mount --bind "$D/libkUEPUser.relaxed.so" /usr/lib/libkUEPUser.so; then
                echo "script check relaxed: signature library covered in memory"
            else
                echo "script check NOT relaxed: bind mount failed"
            fi
        else
            echo "signature library hash mismatch: script check left untouched"
        fi
    else
        echo "no relaxed signature library configured; script check left untouched"
    fi

    # 3. Runtime-mask firmware/app updaters (and bloat units when strip is enabled)
    if [ "$STRIP_ENABLED" = "1" ]; then
        systemctl mask --runtime $U $S >/dev/null 2>&1
        systemctl stop --no-block $U $S >/dev/null 2>&1
    else
        systemctl mask --runtime $U >/dev/null 2>&1
        systemctl stop --no-block $U >/dev/null 2>&1
    fi
    echo "updaters_active=$(systemctl is-active $U 2>/dev/null | grep -c -E '^(active|activating|reloading)') updater_processes=$(ps -eo args 2>/dev/null | grep -i -E 'swu|otasched|vdsystem|upgrade|fw-update|was_back' | grep -v grep | wc -l) strip_units_active=$(systemctl is-active $S 2>/dev/null | grep -c -E '^(active|activating|reloading)')"

    # 4. Post-boot TV Plus sweep (webappservice re-fetches HEPsqFNie0 / tvPlusDeeplinker ~60-90s after boot)
    if [ "$STRIP_ENABLED" = "1" ]; then
        systemd-run --quiet --no-block --collect --unit=codex-tvplus-sweep /bin/sh -c 'sleep 120; app_launcher --kill=HEPsqFNie0.tvplusstandalone >/dev/null 2>&1; swept=""; for p in HEPsqFNie0 com.samsung.tv.tvPlusDeeplinker; do if [ -d "/opt/usr/apps/$p" ]; then pkgcmd -u -n "$p" >/dev/null 2>&1; rm -rf "/opt/usr/home/owner/apps_rw/$p" "/opt/usr/apps/$p"; touch "/opt/usr/apps/$p" && chmod 444 "/opt/usr/apps/$p"; swept="$swept $p"; fi; done; echo "=== $(date "+%Y-%m-%d %H:%M:%S") tvplus_sweep swept=[$swept]" >> /opt/usr/etc/codex/onboot.log 2>&1' >/dev/null 2>&1 || true
    fi

    # 5. Start ExpressVPN Lightway allowlist socket daemon (does NOT connect tunnel automatically)
    if [ "$VPN_CTLD_ENABLED" = "1" ] && [ -x /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld ] && [ ! -f /opt/usr/etc/codex-vpn/disabled ]; then
        systemd-run --quiet --no-block --collect --unit=codex-vpn-ctld-boot /bin/sh -c "
            mkdir -p /run/codex-vpn
            chmod 755 /run/codex-vpn
            chsmack -a _ /run/codex-vpn >/dev/null 2>&1 || true
            chsmack -a _ -t /run/codex-vpn >/dev/null 2>&1 || true
            touch /run/codex-vpn/status.json && chmod 644 /run/codex-vpn/status.json
            chsmack -a _ /run/codex-vpn/status.json >/dev/null 2>&1 || true
            /opt/usr/etc/codex-vpn/status-write.sh >/dev/null 2>&1 || true
            systemctl is-active --quiet codex-vpn-ctld.service 2>/dev/null || /usr/bin/systemd-run --no-ask-password --service-type=simple --unit=codex-vpn-ctld.service --description='CU8000 Lightway VPN Allowlist Socket Controller' -p User=0 -p Group=0 -p SmackProcessLabel=${VPN_HUB_SMACK} -p KillMode=control-group -p SendSIGKILL=yes -p Restart=on-failure -p RestartSec=3 -p LimitCORE=0 -p StandardInput=null /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --serve --socket /run/codex-vpn/ctl.sock --ctl /opt/usr/etc/codex-vpn/ctl.sh --gid ${VPN_USERS_GID} --allowed-uids 0,5001 >/dev/null 2>&1
        " >/dev/null 2>&1 || true
    fi

    # 6. Launch Living Room Hub UI once Wayland settles after boot (disable via: touch /opt/usr/etc/codex-ui/disabled)
    if [ "$HUB_ENABLED" = "1" ] && [ -x /opt/usr/etc/codex-ui/start-hub.sh ] && [ ! -f /opt/usr/etc/codex-ui/disabled ]; then
        systemd-run --quiet --no-block --collect --unit=codex-livingroom-hub-boot /bin/sh -c 'sleep 14; /opt/usr/etc/codex-ui/start-hub.sh' >/dev/null 2>&1 || true
    fi
} >> "$D/onboot.log" 2>&1

cat /proc/sys/kernel/random/boot_id > /tmp/codex-provisioned 2>/dev/null && chmod 644 /tmp/codex-provisioned
tail -n 50 "$D/onboot.log" > /tmp/.codex-log 2>/dev/null && cat /tmp/.codex-log > "$D/onboot.log"; rm -f /tmp/.codex-log
exit 0
