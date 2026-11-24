#!/bin/sh
# Living Room Hub action (UID 5001): cycle selectedLocation (54 <-> 202) without connecting.
exec /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --send "SELECT_NEXT" --timeout 15
