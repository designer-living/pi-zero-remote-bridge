# Remote Status (Phase 2: Bridge) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The bridge sends every key press as a v3 key message, and tells
the gateway when each remote connects, disconnects, and what its battery
level is — read from `bluetoothd` over D-Bus without ever touching the
remote's radio.

**Architecture:** Two new pure-ish modules beside `voice.c`: `packet.c`
(v3 wire encoding + Bluetooth address helpers, fully unit-tested) and
`bluez.c` (a thin basu/sd-bus client: open the system bus, read
`Battery1.Percentage`, report Battery1 signals). `remote_bridge.c` gets one
attach helper and one detach helper that replace today's duplicated
attach/detach code and send the status messages; the bus fd joins the
existing `poll()`.

**Tech Stack:** C (gcc, musl, `-Wall -Wextra`), basu (sd-bus API, linked
statically), Alpine Docker containers for build/test, GitHub Actions + nfpm.

**Spec:** `designer-living/media-control-gateway`
`docs/superpowers/specs/2026-11-24-remote-battery-status-design.md`
(sections "Wire format (shared)" and "Phase 2"; the basu amendment is
gateway PR #131). Locally:
`/home/claude/github/foxy82/media-control-gateway/.claude/worktrees/bridge-cse_01Moswgt9jDJwCqRoWFVBJhH/docs/superpowers/specs/2026-11-24-remote-battery-status-design.md`
on branch `docs/remote-status-basu`.

## Global Constraints

- Wire format, network byte order, version 3 = header `0x30`:
  - prefix `header u8, timestamp_ms u32, type u8` (6 bytes); type 1 = key,
    2 = status.
  - key: prefix + `key_code u16, scan_code u32, value i8` (13 bytes).
  - status: prefix + `kind u8, battery u8, session_ms u32` (12 bytes);
    kind 1 = connected, 2 = disconnected, 3 = battery; battery 0–100 or
    `0xFF` unknown; `session_ms` only on disconnected, else 0.
- **Never drain the remote:** no `org.bluez.Device1` calls, no GATT reads,
  no timers touching the device. Only `Get` of `org.bluez.Battery1`
  `Percentage` (bluetoothd's cached value) and signal subscriptions.
- D-Bus failure must never affect keys: no bus → status still sent with
  battery `0xFF`; ERROR logged once per failure streak; re-open attempted
  only on a device attach, never on a timer.
- Status messages are sent once (no `EDGE_REPEAT_COUNT`); key messages
  keep `EDGE_REPEAT_COUNT` for down/up exactly as today.
- Battery dedupe: a battery message is skipped only if equal to the last
  battery value sent **in this connection**; reset on each attach.
- Bluetooth = `uniq` parses as `xx:xx:xx:xx:xx:xx` (hex, either case).
  Anything else (empty, USB) gets connected/disconnected with `0xFF` only.
- The single `poll()` stays the only wait point.
- Release binary stays fully static; no runtime package dependency.
- Build/test (no local gcc): run inside Alpine, from the repo root:
  `docker run --rm -v "$PWD:/work" -w /work alpine:latest sh -c "apk add -q make gcc musl-dev linux-headers basu-dev basu-static libcap-dev libcap-static pkgconf && make && make test"`
  (Task 1–2 don't need the basu packages yet; including them is harmless.)
  `make clean` afterwards — binaries are root-owned from the container;
  if `make clean` can't delete them, run it in the container too.
- New warnings are bugs (`-Wall -Wextra`).
- Code comments and docs describe the code as it is, not how this branch
  evolved.

## Review Focus

- `uniq` that is almost an address (`18:bf:1c:d3:0b`, `18:bf:1c:d3:0b:a0:ff`,
  `zz:...`) → not Bluetooth, no D-Bus lookup (Task 1 test).
- BlueZ signal for a path whose suffix matches no attached mapping, or a
  longer suffix (`dev_18_..._A0/service0012`) → ignored (Task 1
  `bt_path_has_suffix` tests).
- Battery percentage above 100 from D-Bus → ignored, nothing sent (Task 4
  test).
- Two mappings both attached, battery for one → only that mapping's server
  gets the message (Task 4 test).
- Bus drops mid-run (`sd_bus_process` < 0) → bus closed, ERROR once, keys
  unaffected, next attach re-opens (Task 3: `bluez_process` closes on
  error; covered by on-device check, Task 5).

---

### Task 1: `packet` module — v3 encoding and Bluetooth address helpers

**Files:**
- Create: `packet.h`, `packet.c`
- Create: `tests/test_packet.c`
- Modify: `Makefile` (add `packet.c` to `SOURCE`, build + run
  `tests/test_packet` in `test`, add to `clean`), `.gitignore`
  (`tests/test_packet`)

**Interfaces:**
- Produces (`packet.h`):
  - `#define BRIDGE_PKT_VERSION 3`
  - `enum { PKT_MSG_KEY = 1, PKT_MSG_STATUS = 2 };`
  - `enum { STATUS_CONNECTED = 1, STATUS_DISCONNECTED = 2, STATUS_BATTERY = 3 };`
  - `#define BATTERY_UNKNOWN 0xFF`
  - `#define PKT_KEY_LEN 13`, `#define PKT_STATUS_LEN 12`
  - `size_t packet_build_key(uint8_t *buf, uint32_t ts, uint16_t key_code, uint32_t scan_code, int8_t value);` → 13
  - `size_t packet_build_status(uint8_t *buf, uint32_t ts, uint8_t kind, uint8_t battery, uint32_t session_ms);` → 12
  - `int bt_dev_suffix(const char *uniq, char *out, size_t cap);` →
    0 and `out = "dev_18_BF_1C_D3_0B_A0"` for `"18:bf:1c:d3:0b:a0"`;
    -1 (out untouched) if not an address or `cap` too small.
  - `int bt_path_has_suffix(const char *path, const char *suffix);` → 1 if
    `path` ends with `"/" + suffix`, else 0.

Style: follow `voice.c`/`voice.h` (manual big-endian byte writes into
`buf`, no packed structs).

- [ ] **Step 1: Write the failing tests** in `tests/test_packet.c`, same
  `CHECK` macro and `main` style as `tests/test_voice.c`:

```c
/* key: ts 0x01020304, key 0x0067, scan 0x000C0033, value 1 */
uint8_t k[PKT_KEY_LEN];
CHECK(packet_build_key(k, 0x01020304, 0x0067, 0x000C0033, 1) == 13);
const uint8_t k_exp[] = {0x30, 1,2,3,4, 0x01, 0x00,0x67, 0x00,0x0C,0x00,0x33, 0x01};
CHECK(memcmp(k, k_exp, 13) == 0);
/* value -1 is not valid evdev but must encode as 0xFF, not crash */

/* status: disconnected, battery 39, session 8100 ms */
uint8_t s[PKT_STATUS_LEN];
CHECK(packet_build_status(s, 0x01020304, STATUS_DISCONNECTED, 39, 8100) == 12);
const uint8_t s_exp[] = {0x30, 1,2,3,4, 0x02, 0x02, 39, 0x00,0x00,0x1F,0xA4};
CHECK(memcmp(s, s_exp, 12) == 0);

char out[32];
CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0", out, sizeof out) == 0 && strcmp(out, "dev_18_BF_1C_D3_0B_A0") == 0);
CHECK(bt_dev_suffix("18:BF:1C:D3:0B:A0", out, sizeof out) == 0 && strcmp(out, "dev_18_BF_1C_D3_0B_A0") == 0);
CHECK(bt_dev_suffix("", out, sizeof out) == -1);
CHECK(bt_dev_suffix("18:bf:1c:d3:0b", out, sizeof out) == -1);
CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0:ff", out, sizeof out) == -1);
CHECK(bt_dev_suffix("zz:bf:1c:d3:0b:a0", out, sizeof out) == -1);
CHECK(bt_dev_suffix("usb-3f980000.usb-1.2", out, sizeof out) == -1);
CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0", out, 10) == -1);

CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0", "dev_18_BF_1C_D3_0B_A0") == 1);
CHECK(bt_path_has_suffix("/org/bluez/hci1/dev_18_BF_1C_D3_0B_A0", "dev_18_BF_1C_D3_0B_A0") == 1);
CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0/service0012", "dev_18_BF_1C_D3_0B_A0") == 0);
CHECK(bt_path_has_suffix("/org/bluez/hci0/xdev_18_BF_1C_D3_0B_A0", "dev_18_BF_1C_D3_0B_A0") == 0);
CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_11_22_33_44_55_66", "dev_18_BF_1C_D3_0B_A0") == 0);
```

- [ ] **Step 2: Run to verify it fails**

Run: the Global Constraints container command.
Expected: compile error — `packet.h` not found.

- [ ] **Step 3: Implement** `packet.h` / `packet.c` with the signatures
  above; update `Makefile` and `.gitignore`.

- [ ] **Step 4: Run to verify it passes**

Run: the container command.
Expected: build with no warnings; `test_hidraw`, `test_voice` and
`test_packet` all report pass.

- [ ] **Step 5: Commit**

```bash
git add packet.h packet.c tests/test_packet.c Makefile .gitignore
git commit -m "Add v3 packet encoding and Bluetooth address helpers"
```

### Task 2: Send key presses as v3 key messages

**Files:**
- Modify: `remote_bridge.c` (`send_key_packet`; delete `struct Packet`,
  `PACKET_FORMAT_VERSION`, `PKT_MAKE_HEADER`, `PKT_HEADER_VERSION`)
- Create: `tests/test_bridge.c`
- Modify: `Makefile` (build + run `tests/test_bridge` in `test`: it
  `#include`s `../remote_bridge.c` like `test_hidraw.c`, plus link
  `voice.c packet.c`), `.gitignore`
- Modify: `AGENTS.md` ("Wire format" item 5, the "bump the version"
  sentence in the intro, "Testing / verification"), `README.md` (one line
  saying keys are sent as v3 key messages and that the format lives in
  AGENTS.md is enough)

**Interfaces:**
- Consumes: `packet_build_key`, `PKT_KEY_LEN` (Task 1).
- Produces (test harness in `tests/test_bridge.c`, reused by Task 4):
  - `static int capture_open(Mapping *m);` — binds a UDP socket on
    127.0.0.1 port 0 (non-blocking), points `m->server_addr` at it,
    returns the fd.
  - `static ssize_t capture_read(int fd, uint8_t *buf, size_t cap);` —
    one datagram, or -1 if none waiting.
  - `static Mapping *test_mapping(const char *name);` — `add_mapping`
    with `"127.0.0.1"`, port 1, then returns `&mappings[num_mappings-1]`.

- [ ] **Step 1: Write the failing test**

```c
static void test_key_is_v3(void) {
    Mapping *m = test_mapping("Key Remote");
    int cap = capture_open(m);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    edge_repeat_count = 2;
    send_key_packet(sock, m, 0x0067, 0x000C0033, 1);
    uint8_t buf[64];
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);
    CHECK(buf[0] == 0x30 && buf[5] == PKT_MSG_KEY);
    CHECK(buf[6] == 0x00 && buf[7] == 0x67);                       /* key_code */
    CHECK(buf[8] == 0x00 && buf[9] == 0x0C && buf[10] == 0x00 && buf[11] == 0x33);
    CHECK(buf[12] == 1);
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);      /* edge repeat */
    send_key_packet(sock, m, 0x0067, 0, 2);                        /* hold: once */
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);
    CHECK(capture_read(cap, buf, sizeof buf) == -1);
    edge_repeat_count = 1;
}
```

- [ ] **Step 2: Run to verify it fails** — container command.
  Expected: `test_bridge` FAILs (`buf[0] == 0x30`, length 12 ≠ 13).

- [ ] **Step 3: Implement** — `send_key_packet` builds with
  `packet_build_key` into a `uint8_t[PKT_KEY_LEN]`; keep the TRACE hex
  dump, error and trace logging as they are. Update the docs listed above.

- [ ] **Step 4: Run to verify it passes** — container command. Expected:
  all four test binaries pass, no warnings.

- [ ] **Step 5: Commit**

```bash
git add remote_bridge.c tests/test_bridge.c Makefile .gitignore AGENTS.md README.md
git commit -m "Send key presses as v3 key messages"
```

### Task 3: `bluez` module and basu build plumbing

**Files:**
- Create: `bluez.h`, `bluez.c`
- Modify: `Makefile` (`SOURCE += bluez.c`; `CFLAGS += $(shell pkg-config
  --cflags basu)`; `LDLIBS = $(shell pkg-config --static --libs basu)`
  applied to `remote_bridge`, `tests/test_bridge`; test binaries that
  don't use bluez stay as they are)
- Modify: `.github/workflows/build.yml` and `release.yml` — `apk add`
  gains `basu-dev basu-static libcap-dev libcap-static pkgconf`
- Modify: `AGENTS.md` ("Build": requires basu; the container command;
  "Architecture": the new modules), `README.md` (build-from-source deps
  only, if it lists them)
- Modify: `tests/test_bridge.c` (one no-bus test)

**Interfaces:**
- Produces (`bluez.h`):
  - `int bluez_open(void);` — no-op returning 0 if already open; else
    `sd_bus_open_system` + `sd_bus_match_signal` for
    `org.freedesktop.DBus.Properties.PropertiesChanged` and
    `org.freedesktop.DBus.ObjectManager.InterfacesAdded`, sender
    `org.bluez`. Returns 0 or -1. Logs `LOG_ERROR` on failure only if the
    previous attempt succeeded (or it's the first), so a missing bus logs
    once, not on every attach; logs `LOG_INFO` when a bus opens.
  - `int bluez_fd(void);` — bus fd, or -1 when closed.
  - `short bluez_poll_events(void);` — `sd_bus_get_events` (POLLIN at
    least), 0 when closed.
  - `void bluez_process(void);` — `sd_bus_process` until it returns 0;
    on a negative return, `LOG_ERROR` once and close the bus
    (`sd_bus_flush_close_unref`, fd → -1). Safe when closed.
  - `int bluez_read_battery_at(const char *object_path);` —
    `sd_bus_get_property_trivial(..., "org.bluez.Battery1",
    "Percentage", 'y', ...)`; 0–100, or -1 on any error / >100 / closed.
    DEBUG-log failures only.
  - `int bluez_read_battery(const char *dev_suffix);` — tries
    `/org/bluez/hci0/<suffix>` … `/org/bluez/hci3/<suffix>`, first
    success wins; -1 if none.
  - `void bluez_set_battery_handler(void (*fn)(const char *object_path));`
    — `fn` is called with the signal's object path for:
    PropertiesChanged whose first argument (interface) is
    `"org.bluez.Battery1"`; InterfacesAdded (any; the path is the first
    argument, an object path, not the message path). The handler decides
    whether the path matters.
- `bluez.c` includes `<basu/sd-bus.h>` and logs through the bridge's
  logger: create `log.h` with the `LEVEL_*` values, the `LOG_*` macros,
  `extern int log_level;` and the `log_print` prototype; `remote_bridge.c`
  includes it and drops `static` from `log_level` and `log_print`.
  `log.h` goes in the `git add` below.

- [ ] **Step 1: Write the failing test** (CI and the local container have
  no system bus, which is exactly the "no bus" case):

```c
static void test_bluez_without_bus(void) {
    CHECK(bluez_open() == -1);
    CHECK(bluez_fd() == -1);
    CHECK(bluez_poll_events() == 0);
    bluez_process();                                         /* no crash */
    CHECK(bluez_read_battery("dev_18_BF_1C_D3_0B_A0") == -1);
    CHECK(bluez_read_battery_at("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0") == -1);
}
```

- [ ] **Step 2: Run to verify it fails** — container command. Expected:
  compile error, `bluez.h` not found.

- [ ] **Step 3: Implement** `bluez.c`/`bluez.h`, Makefile, workflows,
  docs. Also confirm the release-style static link works:
  `docker run ... sh -c "apk add -q ... && make CFLAGS='-static -O3 -Wall -Wextra' && ldd remote_bridge"`
  Expected: `ldd` reports not a dynamic executable / no `.so` deps.

- [ ] **Step 4: Run to verify it passes** — container command. Expected:
  all tests pass, no warnings.

- [ ] **Step 5: Commit**

```bash
git add log.h bluez.h bluez.c remote_bridge.c Makefile tests/test_bridge.c .github/workflows/build.yml .github/workflows/release.yml AGENTS.md README.md
git commit -m "Add BlueZ battery reader over basu (static sd-bus)"
```

### Task 4: Connection and battery status messages

**Files:**
- Modify: `remote_bridge.c`
- Modify: `tests/test_bridge.c`
- Modify: `AGENTS.md` (event loop + status messages), `README.md` (a short
  "Battery and connection status" section: what is sent, that it only
  reads bluetoothd's cached value, no config needed)

**Interfaces:**
- Consumes: Task 1 (`packet_build_status`, `bt_dev_suffix`,
  `bt_path_has_suffix`, `STATUS_*`, `BATTERY_UNKNOWN`), Task 2 harness,
  Task 3 (`bluez_*`).
- Produces (in `remote_bridge.c`):
  - `Mapping` fields: `char bt_suffix[32]` (`""` = not Bluetooth),
    `uint8_t battery` (last known, `BATTERY_UNKNOWN`),
    `uint8_t battery_sent` (last sent this connection, `BATTERY_UNKNOWN`),
    `uint64_t connected_ms`. Initialised in `add_mapping`.
  - `static int find_available_mapping(int fd, const char *path, char *uniq_out, size_t uniq_cap);`
    — as today, plus copies the device's `uniq` out.
  - `static void remote_attached(int sock, int m_idx, int fd, const char *path, const char *uniq);`
    — everything both attach sites do today (evfd, dev_path, the
    `hid_*` resets, `voice_open_hidraw`), then: `bt_suffix` from
    `bt_dev_suffix` (else `""`); `connected_ms = now_ms()`;
    `battery_sent = BATTERY_UNKNOWN`; if Bluetooth, `bluez_open()` then
    `bluez_read_battery(bt_suffix)` → `battery` (else
    `BATTERY_UNKNOWN`); send `STATUS_CONNECTED` with `battery`; if
    battery known, set `battery_sent = battery`.
  - `static void remote_detached(int sock, Mapping *m, const char *why);`
    — logs `Remote "%s" disconnected (%s)` as today, `voice_close(...,
    VOICE_END_DISCONNECTED)`, close evfd, clear dev_path; sends
    `STATUS_DISCONNECTED` with `battery` and
    `session_ms = now_ms() - connected_ms` (clamped to `UINT32_MAX`).
    Replaces all four detach sites (POLLHUP/POLLERR, EOF/ENODEV, short
    read, inotify delete; `why` = their current log text).
  - `static void remote_battery(int sock, Mapping *m, int pct);` — ignore
    if `pct < 0 || pct > 100` or `m->evfd < 0`; `battery = pct`; send
    `STATUS_BATTERY` only if `pct != battery_sent`, then
    `battery_sent = pct`.
  - `static void route_battery(int sock, const char *object_path, int pct);`
    — for each attached mapping with a non-empty `bt_suffix` where
    `bt_path_has_suffix(object_path, bt_suffix)`: `remote_battery(sock, m, pct)`.
  - `static void on_bluez_battery(const char *object_path);` — registered
    with `bluez_set_battery_handler`; reads the value once with
    `bluez_read_battery_at(object_path)` only if some attached mapping
    matches the path, then `route_battery(g_sock, object_path, pct)`.
    `g_sock` is a file-scope `static int` set in `main` to the UDP socket.
  - `static void send_status(int sock, Mapping *m, uint8_t kind, uint8_t battery, uint32_t session_ms);`
    — one `packet_build_status` + `sendto` (no repeats), INFO log
    `Status "%s": connected|disconnected|battery (battery N%%|unknown)`.
- Main loop: call `bluez_open()` once at startup (before the initial
  scan) and `bluez_set_battery_handler(on_bluez_battery)`. Each
  iteration: `bluez_process()` before building the poll set; if
  `bluez_fd() >= 0` add it with `bluez_poll_events()`; after `poll`, if
  its `revents` is set call `bluez_process()`. Enlarge `fds[]` by one.

- [ ] **Step 1: Write the failing tests** (`tests/test_bridge.c`; no bus,
  so `bluez_read_battery` returns -1; helper `read_status(cap, &kind,
  &battery, &session_ms)` decodes a 12-byte status message and returns
  its length):

```c
static void test_usb_attach_detach(void) {
    Mapping *m = test_mapping("USB Remote");  int cap = capture_open(m);
    int m_idx = (int)(m - mappings);  g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int fd = open("/dev/null", O_RDONLY);
    remote_attached(g_sock, m_idx, fd, "/dev/input/event9", "");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_CONNECTED && bat == BATTERY_UNKNOWN && sess == 0);
    CHECK(m->bt_suffix[0] == '\0' && m->evfd == fd);
    remote_detached(g_sock, m, "test");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_DISCONNECTED && sess < 1000 && m->evfd == -1);
}

static void test_battery_dedupe_and_reset(void) {
    /* attach with uniq "18:bf:1c:d3:0b:a0" → connected, battery unknown
       (no bus), bt_suffix == "dev_18_BF_1C_D3_0B_A0" */
    remote_battery(g_sock, m, 39);   /* → STATUS_BATTERY 39 */
    remote_battery(g_sock, m, 39);   /* → nothing */
    remote_battery(g_sock, m, 38);   /* → STATUS_BATTERY 38 */
    remote_battery(g_sock, m, 101);  /* → nothing */
    remote_battery(g_sock, m, -1);   /* → nothing */
    /* detach → STATUS_DISCONNECTED battery 38; attach again → connected;
       remote_battery(38) → STATUS_BATTERY 38 (dedupe reset per connection) */
    /* after detach, remote_battery(g_sock, m, 50) → nothing (not attached) */
}

static void test_battery_signal_routes_by_address(void) {
    /* two BT mappings, A "18:bf:1c:d3:0b:a0", B "11:22:33:44:55:66",
       each with its own capture socket, both attached (drain connected) */
    route_battery(g_sock, "/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0", 55);
    /* A's capture: STATUS_BATTERY 55; B's capture: nothing */
    route_battery(g_sock, "/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0/service0012", 60);
    route_battery(g_sock, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", 60);
    /* neither capture gets anything */
}
```

- [ ] **Step 2: Run to verify they fail** — container command. Expected:
  compile errors (`remote_attached`, `route_battery` … undefined).

- [ ] **Step 3: Implement** as in Interfaces; replace both attach sites
  and all four detach sites with the helpers; update docs.

- [ ] **Step 4: Run to verify they pass** — container command, then the
  static release-style build from Task 3 Step 3. Expected: all tests
  pass, no warnings, static binary.

- [ ] **Step 5: Commit**

```bash
git add remote_bridge.c tests/test_bridge.c AGENTS.md README.md
git commit -m "Report remote connect, disconnect and battery to the gateway"
```

### Task 5: Static CI artifacts and on-device check (bedroom Pi)

The bedroom Pi (`pizero-w2-bedroom`, 192.168.5.3, aarch64, Alpine,
reached via the jumpbox `neil@192.168.10.10` whose `~/.ssh/config` has the
host entry) runs the real remote. No ARM emulation is available locally,
so CI builds the test binary.

**Files:**
- Modify: `.github/workflows/build.yml` — build with
  `CFLAGS='-static -O3 -Wall -Wextra'` (as release does) and upload
  `remote_bridge` per arch with `actions/upload-artifact` (name
  `remote_bridge-<arch>`).

- [ ] **Step 1: Commit and push the branch, open the PR** (draft is not
  needed; the user reviews it).

```bash
git add .github/workflows/build.yml
git commit -m "Build static binaries in CI and keep them as artifacts"
git push -u origin feat/remote-status
gh pr create ...   # foxy82 account (gh auth status first)
```

- [ ] **Step 2: Wait for CI, download the arm64 artifact**

Run: `gh run watch` on the PR's build, then
`gh run download <run-id> -n remote_bridge-arm64 -D /tmp/rb-arm64`.
Expected: CI green on all three arches; `/tmp/rb-arm64/remote_bridge`
exists.

- [ ] **Step 3: STOP — ask the user before touching the bedroom Pi.**
  It's their working remote; a bad binary breaks it until rolled back.

- [ ] **Step 4: Install with a backup** (only after the user agrees):

```bash
ssh neil@192.168.10.10 'ssh pizero-w2-bedroom "cp /usr/local/bin/remote_bridge /usr/local/bin/remote_bridge.bak"'
ssh neil@192.168.10.10 'ssh pizero-w2-bedroom "cat > /tmp/remote_bridge.new"' < /tmp/rb-arm64/remote_bridge
ssh neil@192.168.10.10 'ssh pizero-w2-bedroom "chmod 755 /tmp/remote_bridge.new && mv /tmp/remote_bridge.new /usr/local/bin/remote_bridge && rc-service remote-bridge restart"'
```

- [ ] **Step 5: Verify on the Pi and the gateway**

Run: `tail -20 /var/log/remote-bridge.log` on the Pi (expect `Remote
Bridge version`, bus opened, the remote matched, a `Status ... connected
(battery N%)` line once the remote wakes) and
`docker logs --since 10m media-control-gateway | grep bedroom_socket_remote_input`
on prod (`ssh -J neil@192.168.10.10 root@192.168.10.50`). Ask the user to
press a few keys.
Expected: gateway logs `remote connected (battery N%)` and key events
still route (no "unsupported version" warnings).

- [ ] **Step 6: Rollback path if anything is wrong**

```bash
ssh neil@192.168.10.10 'ssh pizero-w2-bedroom "cp /usr/local/bin/remote_bridge.bak /usr/local/bin/remote_bridge && rc-service remote-bridge restart"'
```
