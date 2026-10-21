# Remote Voice Search — Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** For remotes with a configured voice profile, the bridge opens the remote's mic while the voice button is held. It forwards each audio frame to the gateway as a UDP voice packet on a dedicated voice port, and stops sending that button as an ordinary key.

**Architecture:**
- The protocol logic goes in a new `voice.c`/`voice.h`: a profile table, voice packet builders, and the hidraw lookup. It's pure functions, so it can be unit-tested on the build host.
- `remote_bridge.c` gets three additions for each voice-enabled mapping:
  - a hidraw fd in the `poll()` set;
  - mic on/off writes on trigger-key down/up;
  - a 15 s mic cap enforced through the `poll()` timeout.
- **Remote-specific values are data in the profile table, not code.** The Fire TV remote is the first profile. Another remote that works the same way is one more table row, with no code change.

**Tech Stack:** C (gcc, musl/glibc), Linux evdev + hidraw, UDP. Plain `assert`-based unit tests run with `make test`. Packaged by nfpm for Alpine (apk) and Debian.

**Spec:** `media-control-gateway/docs/superpowers/specs/2026-10-19-remote-voice-search-design.md` (in PR #110 of `designer-living/media-control-gateway`)

**Companion plan:** `media-control-gateway/docs/superpowers/plans/2026-10-19-remote-voice-search-gateway.md`. Deploy the gateway first.

## Global Constraints

- **Voice wire format**, big-endian, sent to the remote's `REMOTE` IP on the **voice port** given in its `VOICE=` line (never the key port). Byte 0 is the version, `0x01`.
  - Common header (10 bytes): `header u8`, `msg_type u8`, `session_id u16`, `seq u16`, `timestamp_ms u32`.
  - START, `msg_type` 1: then `codec u8`, `trigger_key u16`. 13 bytes in total.
  - AUDIO, `msg_type` 2: then `codec u8`, then the frame exactly as the remote sent it, without the report-ID byte.
  - END, `msg_type` 3: then `reason u8` (0 = released, 1 = timeout, 2 = disconnected).
  - Codec ids: 1 = Opus.
  - `seq`: START = 0, AUDIO = 1, 2, …, END = last AUDIO seq + 1.
  - START and END are each sent `EDGE_REPEAT_COUNT` times with the **same seq**. AUDIO is sent once.
- **`session_id`** starts from `(uint16_t)now_ms()` at startup and increments per press, so a bridge restart doesn't reuse ids the gateway saw recently.
- **Fire TV profile `firetv`:**
  - trigger key `KEY_SEARCH` (217);
  - mic on = write `{0xF2, 0x01}` to hidraw, mic off = `{0xF2, 0x00}`;
  - audio = input reports with ID `0xF0`, payload = the rest of the report (80 bytes on the tested remote, but **don't hard-code the length**: use the read size);
  - codec Opus.
- **Mic on is only accepted while the button is held:** write it on key-down, never before.
- **Mic cap: 15 s from key-down.** At the cap, write mic off, send END with reason 1, and ignore the matching key-up.
- **A voice-enabled mapping never sends its trigger key as a v2 key packet** (down, up or repeat). The gateway rebuilds the key when voice can't be delivered.
- **If the hidraw node can't be found or opened,** log it at ERROR once per connect. That mapping then behaves exactly as today: the trigger key is sent as a normal key.
- **Voice is opt-in per remote** and off by default. Remotes without a `VOICE=` line behave exactly as today.
- Must build on the existing CI matrix (amd64, arm64, armhf/v6 Alpine) with no new libraries.

## Review Focus

1. **The remote disconnects while the mic is held** (battery, range). Expected: send END with reason 2, close the hidraw fd, and recover cleanly when it reconnects. Test in Task 3: manual step 3.
2. **The hidraw node appears after the evdev node,** or doesn't appear at all (permissions). Expected: keep retrying the lookup on each voice-button press for as long as the mapping has no hidraw fd, and fall back to sending the normal key in the meantime. Test in Task 2: `test_find_hidraw_for_event_returns_minus1_when_missing`, plus the Task 3 manual step.
3. **Key-repeat events** (value 2) arrive while the button is held. Expected: they are neither forwarded as keys nor treated as a new press. Test in Task 3, manual step 2: at TRACE, no key packets while the button is held.
4. **Reports other than audio arrive on hidraw** (key reports such as `02 21 02 …`, which also reach evdev). Expected: ignored on hidraw, and never sent as audio. Test in Task 1: `test_audio_payload_ignores_other_report_ids`.
5. **A second remote on the same Pi has no voice profile.** Expected: its trigger key keeps working as an ordinary key. Covered by the "Voice is opt-in" constraint, and checked in the Task 3 manual step with the Amazon/AR remotes paired on the dev Pi.

---

### Task 1: `voice.c` — profiles and voice packet builders, with unit tests

**Files:**
- Create: `voice.h`, `voice.c`
- Create: `tests/test_voice.c`
- Modify: `Makefile`:
  - `SOURCE = remote_bridge.c voice.c`;
  - add a `test` target that builds `tests/test_voice.c voice.c` into `tests/test_voice` and runs it;
  - extend `clean`.
- Modify: `.github/workflows/build.yml` (run `make test` after `make` inside the same Alpine container) and `.gitignore` (`tests/test_voice`)

**Interfaces:**
- Produces, in `voice.h`:

```c
#define VOICE_PKT_VERSION 1
enum { VOICE_MSG_START = 1, VOICE_MSG_AUDIO = 2, VOICE_MSG_END = 3 };
enum { VOICE_CODEC_OPUS = 1 };
enum { VOICE_END_RELEASED = 0, VOICE_END_TIMEOUT = 1, VOICE_END_DISCONNECTED = 2 };
#define VOICE_HDR_LEN 10
#define VOICE_MAX_PKT 300

typedef struct {
    const char *name;            /* matched against VOICE= config, e.g. "firetv" */
    uint16_t    trigger_key;     /* evdev key code */
    uint8_t     mic_on[4];  size_t mic_on_len;
    uint8_t     mic_off[4]; size_t mic_off_len;
    uint8_t     audio_report_id;
    uint8_t     codec;
} VoiceProfile;

const VoiceProfile *voice_profile_find(const char *name);   /* NULL if unknown */
size_t voice_build_start(uint8_t *buf, uint16_t sid, uint32_t ts, uint8_t codec, uint16_t trigger_key);
size_t voice_build_audio(uint8_t *buf, size_t cap, uint16_t sid, uint16_t seq, uint32_t ts,
                         uint8_t codec, const uint8_t *frame, size_t frame_len);   /* 0 if it won't fit */
size_t voice_build_end(uint8_t *buf, uint16_t sid, uint16_t seq, uint32_t ts, uint8_t reason);
/* Returns the payload pointer/len if report is an audio report for profile, else NULL. */
const uint8_t *voice_audio_payload(const VoiceProfile *p, const uint8_t *report, size_t len, size_t *out_len);
```

- [ ] **Step 1: Write the failing tests in `tests/test_voice.c`**

These are plain `assert`s in `main()`, printing `"all voice tests passed"`.

```c
test_firetv_profile_values();      /* trigger 217, mic_on F2 01, mic_off F2 00, audio id 0xF0, codec 1 */
test_unknown_profile_is_null();
test_build_start_layout();         /* 01 01 sid seq=0000 ts(4) 01 00D9 -> 13 bytes, big-endian */
test_build_audio_layout();         /* 01 02 sid seq ts 01 + frame; returns 11+frame_len */
test_build_audio_too_big_returns_0();
test_build_end_layout();           /* 01 03 sid seq ts reason -> 11 bytes */
test_audio_payload_strips_report_id();      /* F0 + 80 bytes -> ptr to byte 1, len 80 */
test_audio_payload_ignores_other_report_ids();   /* 02 21 02 00 00 -> NULL (Review Focus 4) */
```

- [ ] **Step 2: Run the tests and check they fail**

Run: `make test`
Expected: FAIL at compile time, with `voice.h: No such file`

- [ ] **Step 3: Implement `voice.c`**

Hold the profiles in a `static const VoiceProfile profiles[]` table, with the `firetv` row from Global Constraints. Write the multi-byte fields with explicit big-endian shifts, not struct packing.

- [ ] **Step 4: Run the tests and the build**

Run: `make test && make`
Expected: `all voice tests passed`, and `remote_bridge` builds with no warnings under `-Wall -Wextra`

- [ ] **Step 5: Commit**

```bash
git add voice.h voice.c tests/test_voice.c Makefile .github/workflows/build.yml .gitignore
git commit -m "feat(voice): profile table and voice packet builders"
```

---

### Task 2: Config and hidraw discovery

**Files:**
- Modify: `voice.c` / `voice.h` (add discovery)
- Modify: `remote_bridge.c`:
  - `Mapping` gets `const VoiceProfile *voice; int hidfd; int mic_open; uint16_t sid; uint16_t seq; uint64_t mic_opened_ms;`;
  - `Mapping` also gets `struct sockaddr_in voice_addr`: the mapping's server IP with the voice port;
  - `load_config` parses `VOICE=<exact_remote_name>,<profile>,<voice_port>`, which applies to every mapping with that name. An unknown profile name, or a missing or invalid port, logs an ERROR and the line is ignored. Process `VOICE=` lines after all `REMOTE=` lines, so their order in the file doesn't matter;
  - initialise `hidfd = -1` in `add_mapping`.
- Modify: `remote-bridge.conf`: document `VOICE=`, with a commented example `# VOICE="AR,firetv,9100"`.
- Modify: `99-remote-bridge.rules`: add `SUBSYSTEM=="hidraw", GROUP="input", MODE="0660"`.
- Modify: `packaging/scripts/postinstall-apk.sh`: extend the mdev note with `hidraw[0-9]+ root:input 660`, and also trigger `--subsystem-match=hidraw`. Do the same in `postinstall-deb.sh`.
- Modify: `README.md`: a short "Voice remotes" section covering the `VOICE=` line, the hidraw permission requirement, and the `firetv` profile.
- Test: `tests/test_voice.c`

**Interfaces:**
- Produces:
  - `int voice_find_hidraw_for_event(const char *sysfs_root, const char *event_name, char *out_path, size_t cap);`
    - Scan `<sysfs_root>/class/hidraw/hidraw*/device/input/input*/` for a directory entry named `event_name` (for example `event0`).
    - On a match, write `/dev/hidrawN` to `out_path` and return 0. Return -1 if there's no match.
    - `sysfs_root` is `/sys` in production, and a temp dir tree in the tests.

- [ ] **Step 1: Write the failing tests**

```c
test_find_hidraw_for_event_matches_sibling();      /* build tmp tree hidraw3/device/input/input1/event7 -> "/dev/hidraw3" */
test_find_hidraw_for_event_returns_minus1_when_missing();   /* Review Focus 2 */
```

- [ ] **Step 2: Run the tests and check they fail**

Run: `make test`
Expected: FAIL (undefined `voice_find_hidraw_for_event`)

- [ ] **Step 3: Implement the discovery and the `VOICE=` parsing**

When an evdev node is matched to a mapping with a profile, look up its hidraw node by basename (`event0`), open it `O_RDWR | O_NONBLOCK`, and store it in `hidfd`. If the lookup or open fails, log an ERROR and leave `hidfd = -1`.

- [ ] **Step 4: Run the tests and the build**

Run: `make test && make`
Expected: pass, with no warnings

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(voice): VOICE= config and hidraw discovery"
```

---

### Task 3: Main-loop integration (mic control, audio forwarding, cap)

**Files:**
- Modify: `remote_bridge.c`

**Interfaces:**
- Consumes: everything from Tasks 1 and 2

The behaviour, per mapping `m` with `m->voice != NULL`:
- **EV_KEY with code `m->voice->trigger_key`:**
  - **If `hidfd < 0`:** retry the hidraw lookup once (Review Focus 2). If it still fails, fall through to the normal key-packet path.
  - **value 1 (key-down):**
    - write `mic_on` to `hidfd`;
    - set `mic_open = 1`, `mic_opened_ms = now`, `seq = 0`, and `sid = ++next_sid`;
    - send START `edge_repeat_count` times;
    - don't send a key packet.
  - **value 2 (repeat):** ignore it (Review Focus 3).
  - **value 0 (key-up):**
    - if `mic_open`: write `mic_off`, send END (reason 0, seq = `seq + 1`) `edge_repeat_count` times, and set `mic_open = 0`;
    - if not (for example after the cap fired): ignore it;
    - in both cases, don't send a key packet.
- **`hidfd` in `poll()` (POLLIN):**
  - read reports in a loop until `EAGAIN`, one `read()` per report into a 256-byte buffer;
  - if `mic_open` and `voice_audio_payload(...)` returns a payload, send AUDIO with `++seq`;
  - on POLLHUP/POLLERR/ENODEV, or when the evdev side disconnects: close it, set `hidfd = -1`, and if `mic_open` send END reason 2 (Review Focus 1).
- **Cap:**
  - `poll()` timeout = the milliseconds until the earliest open mic reaches 15000 ms (`-1` if none);
  - when it expires: write `mic_off`, send END reason 1, and set `mic_open = 0`.
- **Logging:**
  - INFO: "voice start/end sid=… frames=… reason=…";
  - TRACE: per-packet hex, as the key path does now.

- [ ] **Step 1: Implement the behaviour above**

Keep the voice handling in small static helpers in `remote_bridge.c`:
- `voice_on_key(Mapping*, int value, int sock)`, which sends to `m->voice_addr`, returns 1 if it consumed the event;
- `voice_on_hidraw(Mapping*, int sock)`;
- `voice_close(Mapping*, int sock, uint8_t reason)`;
- `voice_next_timeout_ms(void)`.

- [ ] **Step 2: Build and run the unit tests**

Run: `make test && make`
Expected: pass, with no warnings

- [ ] **Step 3: Manual hardware check on `pizero-w1-dev-1` (192.168.5.113)**

1. Build an armhf apk, or `make` on the Pi with `apk add make gcc musl-dev linux-headers`. Install it, add `VOICE="AR,firetv,9100"` to `/etc/remote-bridge.conf` (the dev remote's gateway voice input, `dev_voice_input`), add the mdev line `hidraw[0-9]+ root:input 660`, re-trigger mdev or reboot, and persist with `lbu commit`.
2. Make sure the gateway from the companion plan is deployed and the dev input's matrix port 4 is on input 6. Hold the mic, say "Stranger Things", and let go. Expected:
   - the LED goes white;
   - the Chromecast shows the results;
   - the bridge log shows the voice start and end with a frame count;
   - no `KEY_SEARCH` v2 packet is sent (check at TRACE).
3. Hold the mic and switch the remote off, or take it out of range. Expected: END reason 2 is logged, and after reconnecting a new hold works (Review Focus 1).
4. Hold the mic for more than 15 s. Expected: the LED stops, END reason 1 is logged, and the later release is ignored.
5. Quick tap. Expected: START and END are sent with no AUDIO, and nothing happens on the TV.
6. Press other buttons, then the Search button of a remote paired without a `VOICE=` line. Expected: they behave exactly as before (Review Focus 5).

- [ ] **Step 4: Commit and open a PR**

```bash
git commit -am "feat(voice): forward remote mic audio as voice packets"
```
