#ifndef VOICE_H
#define VOICE_H

#include <stddef.h>
#include <stdint.h>

/* -------- Voice packet wire format (big-endian, own UDP port) --------
 * Common header (10 bytes): version u8, msg_type u8, session_id u16,
 *                           seq u16, timestamp_ms u32
 * START: + codec u8 + trigger_key u16           (13 bytes)
 * AUDIO: + codec u8 + frame as the remote sent it
 * END:   + reason u8                            (11 bytes)
 */
#define VOICE_PKT_VERSION 1
enum { VOICE_MSG_START = 1, VOICE_MSG_AUDIO = 2, VOICE_MSG_END = 3 };
enum { VOICE_CODEC_OPUS = 1 };
enum { VOICE_END_RELEASED = 0, VOICE_END_TIMEOUT = 1, VOICE_END_DISCONNECTED = 2 };
#define VOICE_HDR_LEN 10
#define VOICE_MAX_PKT 300

/* Everything remote-specific about voice capture. A remote that works the
 * same way as an existing one is a new row in the profile table. */
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

/* Find the hidraw node belonging to the same HID device as evdev node
 * `event_name` (e.g. "event0") by scanning
 * <sysfs_root>/class/hidraw/hidrawN/device/input/inputM/<event_name>.
 * On a match writes "/dev/hidrawN" to out_path and returns 0; else -1. */
int voice_find_hidraw_for_event(const char *sysfs_root, const char *event_name, char *out_path, size_t cap);

/* What to do with a voice-trigger key event (value 0=up, 1=down, 2=repeat).
 * voice_owned: this press's down was taken by voice. hid_ready: hidraw open.
 * A press is either all voice or all normal key, so a down and its up never split. */
typedef enum { VOICE_KEY_PASS, VOICE_KEY_START, VOICE_KEY_SWALLOW, VOICE_KEY_END } VoiceKeyAction;
VoiceKeyAction voice_key_action(int value, int voice_owned, int hid_ready);

#endif
