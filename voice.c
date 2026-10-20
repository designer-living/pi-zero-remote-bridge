#include "voice.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* -------- Voice profiles -------- */
static const VoiceProfile profiles[] = {
    /* Amazon Fire TV voice remote: the host opens the mic by writing HID
     * output report F2 01 while the Search (mic) button is held, closes it
     * with F2 00; audio arrives as input report F0, one Opus frame each. */
    {
        .name = "firetv",
        .trigger_key = 217, /* KEY_SEARCH */
        .mic_on = {0xF2, 0x01}, .mic_on_len = 2,
        .mic_off = {0xF2, 0x00}, .mic_off_len = 2,
        .audio_report_id = 0xF0,
        .codec = VOICE_CODEC_OPUS,
    },
};

const VoiceProfile *voice_profile_find(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (strcmp(profiles[i].name, name) == 0) return &profiles[i];
    }
    return NULL;
}

/* -------- Packet builders -------- */
static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_header(uint8_t *buf, uint8_t msg_type, uint16_t sid, uint16_t seq, uint32_t ts) {
    buf[0] = VOICE_PKT_VERSION;
    buf[1] = msg_type;
    put_u16(buf + 2, sid);
    put_u16(buf + 4, seq);
    put_u32(buf + 6, ts);
}

size_t voice_build_start(uint8_t *buf, uint16_t sid, uint32_t ts, uint8_t codec, uint16_t trigger_key) {
    put_header(buf, VOICE_MSG_START, sid, 0, ts);
    buf[VOICE_HDR_LEN] = codec;
    put_u16(buf + VOICE_HDR_LEN + 1, trigger_key);
    return VOICE_HDR_LEN + 3;
}

size_t voice_build_audio(uint8_t *buf, size_t cap, uint16_t sid, uint16_t seq, uint32_t ts,
                         uint8_t codec, const uint8_t *frame, size_t frame_len) {
    size_t len = VOICE_HDR_LEN + 1 + frame_len;
    if (len > cap) return 0;
    put_header(buf, VOICE_MSG_AUDIO, sid, seq, ts);
    buf[VOICE_HDR_LEN] = codec;
    memcpy(buf + VOICE_HDR_LEN + 1, frame, frame_len);
    return len;
}

size_t voice_build_end(uint8_t *buf, uint16_t sid, uint16_t seq, uint32_t ts, uint8_t reason) {
    put_header(buf, VOICE_MSG_END, sid, seq, ts);
    buf[VOICE_HDR_LEN] = reason;
    return VOICE_HDR_LEN + 1;
}

const uint8_t *voice_audio_payload(const VoiceProfile *p, const uint8_t *report, size_t len, size_t *out_len) {
    if (!p || !report || len < 2 || report[0] != p->audio_report_id) return NULL;
    *out_len = len - 1;
    return report + 1;
}

/* -------- hidraw discovery -------- */
static int input_dir_has_event(const char *input_root, const char *event_name) {
    DIR *d = opendir(input_root);
    if (!d) return 0;
    struct dirent *ent;
    int found = 0;
    while (!found && (ent = readdir(d))) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char path[1200];  /* input_root + / + d_name + / + event_name */
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s/%s", input_root, ent->d_name, event_name);
        if (stat(path, &st) == 0) found = 1;
    }
    closedir(d);
    return found;
}

int voice_find_hidraw_for_event(const char *sysfs_root, const char *event_name, char *out_path, size_t cap) {
    char class_dir[256];
    snprintf(class_dir, sizeof(class_dir), "%s/class/hidraw", sysfs_root);
    DIR *d = opendir(class_dir);
    if (!d) return -1;
    struct dirent *ent;
    int rc = -1;
    while (rc != 0 && (ent = readdir(d))) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
        char input_root[600];  /* class_dir + / + d_name + /device/input */
        snprintf(input_root, sizeof(input_root), "%s/%s/device/input", class_dir, ent->d_name);
        if (input_dir_has_event(input_root, event_name)) {
            snprintf(out_path, cap, "/dev/%s", ent->d_name);
            rc = 0;
        }
    }
    closedir(d);
    return rc;
}

/* -------- Trigger-key decision -------- */
VoiceKeyAction voice_key_action(int value, int voice_owned, int hid_ready) {
    if (voice_owned) return value == 0 ? VOICE_KEY_END : VOICE_KEY_SWALLOW;
    if (value == 1 && hid_ready) return VOICE_KEY_START;
    return VOICE_KEY_PASS;
}
