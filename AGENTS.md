# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository.

## What this is

`remote_bridge` is a small single-file C daemon that runs on a Raspberry Pi
Zero (or any Linux box) with an IR/Bluetooth remote receiver attached. It
reads `EV_KEY` events from `/dev/input/event*`, matches devices by exact
name, and forwards each key event as a compact 12-byte binary UDP packet to
a configured server. It handles hotplug (USB/BT dongles connecting and
disconnecting) via `inotify` and can service multiple remotes at once, each
mapped to its own destination IP/port.

**This repo is the sender half of a pair.** The receiving end — the service
that listens on the UDP port and decodes packets into actions — lives in
[designer-living/media-control-gateway](https://github.com/designer-living/media-control-gateway).
When changing the wire format (`struct Packet` / `PACKET_FORMAT_VERSION` in
`remote_bridge.c`), the two repos must be kept in sync — bump
`PACKET_FORMAT_VERSION` and check that the gateway's decoder handles the new
header correctly.

## Build

```bash
make                          # builds ./remote_bridge (VERSION=dev)
make VERSION=1.2.3            # embed a specific version string
make clean
```

Requires `gcc` and standard Linux headers (`linux/input.h`). There is no
separate lint step; `CFLAGS` already includes `-Wall -Wextra` (see
`Makefile`) — treat new warnings as bugs to fix.

## Testing / verification

There is no unit test suite — this is a small daemon that talks to real
kernel input devices and a real network socket, so verification is manual:

```bash
# Find the exact evdev name of the remote (must match config exactly)
sudo evtest

# Run with verbose logging against a real device, no install needed
./remote_bridge "Your Remote Name" 192.168.1.100 9999 TRACE

# Or via a config file (multi-remote mode)
./remote_bridge -c remote-bridge.conf

# On the receiving side, confirm packets arrive (no gateway needed for this check)
nc -u -l -p 9999 | xxd
```

`TRACE` log level dumps the raw hex bytes of every packet sent — use it when
debugging wire-format changes.

CI (`.github/workflows/build.yml`) cross-builds for `amd64`, `arm64`, and
`armhf` (ARMv6, for Pi Zero v1) on every PR using QEMU + an Alpine Docker
container, purely as a compile check. `.github/workflows/release.yml` does
the same on GitHub release creation, then packages the static binaries with
`nfpm` (`packaging/nfpm-deb.yaml`, `packaging/nfpm-apk.yaml`) and attaches
`.deb`/`.apk`/raw binaries to the release.

## Architecture

Everything lives in `remote_bridge.c` (single file, no dependencies beyond
libc). The flow:

1. **Config loading** (`load_config` / CLI args) builds a list of
   `Mapping` structs (`mappings[]`, max `MAX_MAPPINGS = 32`), each with a
   target remote name, destination `sockaddr_in`, and an optional repeat
   throttle delay. Two invocation modes: legacy positional args (single
   remote) or `-c <file>` (multi-remote, `REMOTE="name,ip,port,delay"`
   lines).
2. **Device discovery**: on startup, scans `/dev/input/event*`, opens each,
   reads its name via `EVIOCGNAME`, and matches it against unmapped
   `Mapping` entries (`find_available_mapping`). Matched devices' fds are
   stored on the mapping.
3. **Hotplug**: an `inotify` watch on `/dev/input` catches `IN_CREATE` /
   `IN_ATTRIB` / `IN_MOVED_TO` (device appears — re-run the matching logic)
   and `IN_DELETE` (device removed — clear the mapping's fd). Devices can
   also disconnect mid-read (`POLLHUP`/`POLLERR`, EOF, or `ENODEV`), which is
   handled the same way so a remote can drop and reconnect without a
   restart.
4. **Event loop**: a single `poll()` over all currently-open mapping fds
   plus the inotify fd. Each mapping tracks the most recent `EV_MSC`/
   `MSC_SCAN` value it has seen (`pending_scan_code`) — devices that can't
   express a button as a standard evdev keycode (e.g. some Bluetooth remotes
   report `KEY_UNKNOWN` for several distinct buttons) still emit a unique
   raw scan code just before the `EV_KEY` event, so this lets those buttons
   be told apart downstream. For each `EV_KEY` event, optionally throttles
   `value == 2` (repeat) events per-mapping using `repeat_delay_ms`, then
   builds a `struct Packet` (consuming and clearing `pending_scan_code`) and
   `sendto()`s it to that mapping's server.
5. **Wire format** — `struct Packet` (packed, 12 bytes):
   - `header` (1 byte): upper 4 bits = `PACKET_FORMAT_VERSION`, lower 4
     bits reserved. Bump the version and update both sides if you change
     the layout below.
   - `timestamp_ms` (4 bytes, network order): low 32 bits of a monotonic
     ms clock.
   - `key_code` (2 bytes, network order): the evdev key code.
   - `scan_code` (4 bytes, network order): the raw `MSC_SCAN` value that
     preceded this key event, or `0` if none was seen. Lets the gateway
     disambiguate devices/buttons that share `KEY_UNKNOWN` (240).
   - `value` (1 byte): `0` = up, `1` = down, `2` = repeat.
6. **Logging**: `log_print` / `LOG_ERROR|INFO|DEBUG|TRACE` macros print UTC
   timestamps to stdout (captured by journald/OpenRC, not written to a file
   directly by the program). Level is set via `LOG_LEVEL` in the config
   file or the 4th CLI arg.

Everything is static/global state (`mappings[]`, `num_mappings`,
`log_level`) — this is a single-threaded, single-purpose daemon, not a
library; keep additions in that spirit rather than introducing abstraction
layers.

## Packaging / deployment

- `Makefile`'s `install`/`uninstall` targets create a dedicated
  `remote-bridge` system user (in the `input` group), install the udev rule
  (`99-remote-bridge.rules`), and set up systemd (`remote-bridge.service`)
  or OpenRC (`remote-bridge.initd`) depending on what's detected. These run
  `sudo` commands and mutate live system state — do not run them as part of
  routine development.
- `packaging/nfpm-*.yaml` + `packaging/scripts/*` define the `.deb`/`.apk`
  packages built by the release workflow; `postinstall`/`preremove` scripts
  mirror what the Makefile's install/uninstall targets do.
- Config file format and operational runbook (service management, logs,
  troubleshooting) are documented in `README.md` — keep both in sync if you
  change config keys or CLI args.
