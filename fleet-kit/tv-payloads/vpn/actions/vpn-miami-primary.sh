#!/bin/sh
# Living Room Hub action (UID 5001): connect/switch to USA - Miami (54).
exec /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --send "SWITCH 54" --timeout 90
