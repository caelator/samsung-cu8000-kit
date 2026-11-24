# Building From Source & Packaging a Distribution Zip

This repository contains **source code, scripts, generic ASTC assets, and documentation only**. To build a self-contained macOS distribution zip (or deploy directly from your Mac), assemble `./local-runtime/` using the steps below.

---

## 1. Prerequisites (macOS Build Machine)

- **Zig** (`brew install zig`) — zero-sysroot ARMv7 EABI5 softfp C cross-compiler for `living-room-hub-native` and `codex-vpn-ctld`
- **FreeType headers** (`brew install freetype`) — compile-time headers only (`living-room-hub-native` loads `/usr/lib/libfreetype.so.6` dynamically on the TV via `dlopen`)
- **Python 3.9+** (macOS built-in `/usr/bin/python3` works out of the box)
- **Optional (only if regenerating ASTC textures):** Swift (Xcode Command Line Tools) + `astcenc` (`brew install astc-encoder`)

---

## 2. Obtain External Runtime Dependencies (Not Redistributed Here)

Create a local working directory (ignored by git):

```bash
mkdir -p local-runtime/{python-lib,root-payloads,ui,vpn,onboot}
```

### A. Samsung `sdb` (Smart Development Bridge)
Install Tizen Studio (or CLI tools) from Samsung/Tizen and copy the macOS `sdb` binary:

```bash
cp /path/to/tizen-studio/tools/sdb local-runtime/sdb
chmod +x local-runtime/sdb
```

### B. `samsung-tv-root` (Unlicense)
Download release `v0.0.12` (or newer) of [chris-ritsen/samsung-tv-root](https://github.com/chris-ritsen/samsung-tv-root):

```bash
# Copy the Python source package (zero external dependencies)
cp -R /path/to/samsung-tv-root/src/samsung_tv_root local-runtime/python-lib/

# Copy the two TV root payloads from the official release archive
cp /path/to/samsung-tv-root-release/_internal/payloads/common/SamsungTvArchiveRoot.dll local-runtime/root-payloads/
cp /path/to/samsung-tv-root-release/_internal/payloads/common/SamsungTvRootAgent.dll local-runtime/root-payloads/
```

---

## 3. Cross-Compile Native ARMv7 TV Binaries

Both binaries target **ARMv7 EABI5 softfp (`e_flags = 0x05000200`)** matching Tizen 9 `/lib/ld-linux.so.3`:

### A. `living-room-hub-native` (Bare-Metal Wayland + Mali-G52 GLES2 + Media OS)
```bash
FREETYPE_INC="$(brew --prefix freetype)/include/freetype2"

zig cc -target arm-linux-gnueabi.2.28 -mcpu=cortex_a53+neon -O2 -s \
  -Wall -Wextra -Wno-unused-parameter \
  -I"$FREETYPE_INC" \
  tv-hub/native-c/main.c \
  tv-hub/native-c/renderer.c \
  tv-hub/native-c/ffi_loader.c \
  tv-hub/media-c/media_os.c \
  tv-hub/media-c/http_client.c \
  tv-hub/media-c/json_mini.c \
  tv-hub/media-c/astc_pool.c \
  tv-hub/media-c/tizen_player.c \
  -o local-runtime/ui/living-room-hub-native \
  -ldl -lpthread -lm
```

### B. `codex-vpn-ctld` (Static ARMv7 Unix Socket Allowlist Controller)
```bash
zig cc -target arm-linux-musleabi -mcpu=cortex_a53 -O2 -s -static \
  -Wall -Wextra \
  vpn-controller/codex-vpn-ctld.c \
  -o local-runtime/vpn/codex-vpn-ctld
```

### C. Optional: Re-bake Generic ASTC Textures
Pre-baked generic `Plex` / `YouTube` / `HDMI 1` ASTC textures are already included under `tv-hub/assets/`. To customize or re-bake them:

```bash
swift tv-hub/bake_assets.swift
astcenc -cl tv-hub/assets_png/bg_base.png tv-hub/assets/bg_base.astc 6x6 -medium
for f in card_plex_idle card_plex_focus card_youtube_idle card_youtube_focus card_switch2_idle card_switch2_focus dock_rail icons_atlas; do
  astcenc -cl "tv-hub/assets_png/${f}.png" "tv-hub/assets/${f}.astc" 4x4 -medium
done
for f in halo_plex halo_youtube halo_switch2; do
  astcenc -cl "tv-hub/assets_png/${f}.png" "tv-hub/assets/${f}.astc" 6x6 -medium
done
rm -rf tv-hub/assets_png
```

---

## 4. Optional: ExpressVPN Lightway ARMv7 Client

If you want on-TV Lightway VPN support:

1. Cross-compile [ExpressVPN/lightway-core](https://github.com/expressvpn/lightway-core) (GPLv2) as a static ARMv7 musl `noasm` softfp executable (`lightway-client-armv7-noasm`), and place it at `local-runtime/vpn/lightway-client`.
2. Place ExpressVPN's public Lightway CA PEM certificate at `local-runtime/vpn/ca.pem`.
3. Copy `fleet-kit/tv-payloads/vpn/endpoints.example.tsv` → `local-runtime/vpn/endpoints.tsv` and `locations.example.json` → `local-runtime/vpn/locations.json`, and populate them with genuine ExpressVPN Lightway endpoints from your account (see [docs/VPN-AND-GATEWAY.md](VPN-AND-GATEWAY.md)).
4. Update `CLIENT_SHA` and `CA_SHA` in `fleet-kit/tv-payloads/vpn/wrapper.sh` to pin your compiled binary's SHA-256.

---

## 5. Optional: Local Anchor-Gated Kernel Module (`kernel-uep/`)

Because Samsung's kernel enforces module `vermagic` and exact RAM virtual addresses per firmware build, compiled `.ko` files are **not** distributed in this repository.

To build them locally for your own verified CU8000 firmware (`0090` or `0080`):

1. Pull your own TV's `/lib/modules/$(uname -r)/kernel/crypto/arc4.ko` to `evidence/firmware/stock-arc4.ko` (for `vermagic` skeleton cloning only).
2. Assemble the anchor-gated ARM32 object with `clang` / `zig cc -target arm-linux-gnueabi -c`:
   ```bash
   # Example for build 0090 (NEWVALUE=0 clears enforcement in RAM; NEWVALUE=1 restores it):
   zig cc -target arm-linux-gnueabi -marm -c -DNEWVALUE=0 -x assembler-with-cpp kernel-uep/uepwrite5.S -o uepclear5_raw.o
   python3 kernel-uep/build_uep_module.py uepclear5_raw.o local-runtime/onboot/uepclear5.ko uepclear5

   zig cc -target arm-linux-gnueabi -marm -c -DNEWVALUE=1 -x assembler-with-cpp kernel-uep/uepwrite5.S -o ueprestore5_raw.o
   python3 kernel-uep/build_uep_module.py ueprestore5_raw.o local-runtime/onboot/ueprestore5.ko ueprestore5
   ```
3. Never reuse kernel offsets across different firmware builds — both `.S` templates verify two 16-byte code anchors and abort cleanly with `-ENODEV` on any mismatch.

---

## 6. Build the Shareable macOS Distribution Zip

Once `./local-runtime/` is populated, run one command:

```bash
python3 fleet-kit/build_package.py --runtime-dir ./local-runtime --version v1.0.0
```

Before emitting `dist/cu8000-property-kit-v1.0.0.zip`, the builder enforces:
- Zero private files (`.private`, `.env`, `client.yaml`) or `--deny` strings
- Shell syntax check (`sh -n` / `bash -n`) on every TV and macOS script
- macOS built-in Python 3.9+ import check inside the staged folder
- ARMv7 EABI5 softfp (`0x05000200`) ELF audit on compiled native binaries
- Full `manifest.json` + `SHA256SUMS.txt`

---

## 7. Run Local Verification Tests

```bash
python3 fleet-kit/test_fleet_kit.py
```

If you also want to run the gateway unit tests locally:

```bash
python3 -m pytest gateway/tests/
```
