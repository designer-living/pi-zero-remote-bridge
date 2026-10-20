#include "voice.h"

#include <string.h>

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
