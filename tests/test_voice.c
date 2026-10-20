/* Unit tests for voice.c (profile table and voice packet builders).
 * Plain asserts; run with `make test`. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../voice.h"

static void test_firetv_profile_values(void) {
    const VoiceProfile *p = voice_profile_find("firetv");
    assert(p != NULL);
    assert(p->trigger_key == 217);
    assert(p->mic_on_len == 2 && p->mic_on[0] == 0xF2 && p->mic_on[1] == 0x01);
    assert(p->mic_off_len == 2 && p->mic_off[0] == 0xF2 && p->mic_off[1] == 0x00);
    assert(p->audio_report_id == 0xF0);
    assert(p->codec == VOICE_CODEC_OPUS);
}

static void test_unknown_profile_is_null(void) {
    assert(voice_profile_find("nope") == NULL);
    assert(voice_profile_find("") == NULL);
}

static void test_build_start_layout(void) {
    uint8_t buf[VOICE_MAX_PKT];
    size_t n = voice_build_start(buf, 0x1234, 0xA1B2C3D4, VOICE_CODEC_OPUS, 217);
    const uint8_t want[] = {0x01, 0x01, 0x12, 0x34, 0x00, 0x00,
                            0xA1, 0xB2, 0xC3, 0xD4, 0x01, 0x00, 0xD9};
    assert(n == 13);
    assert(memcmp(buf, want, sizeof(want)) == 0);
}

static void test_build_audio_layout(void) {
    uint8_t buf[VOICE_MAX_PKT];
    const uint8_t frame[] = {0xB8, 0x01, 0x02};
    size_t n = voice_build_audio(buf, sizeof(buf), 0x0007, 0x0102, 0x00000010,
                                 VOICE_CODEC_OPUS, frame, sizeof(frame));
    const uint8_t want[] = {0x01, 0x02, 0x00, 0x07, 0x01, 0x02,
                            0x00, 0x00, 0x00, 0x10, 0x01, 0xB8, 0x01, 0x02};
    assert(n == 11 + sizeof(frame));
    assert(memcmp(buf, want, sizeof(want)) == 0);
}

static void test_build_audio_too_big_returns_0(void) {
    uint8_t buf[20];
    uint8_t frame[10] = {0};
    assert(voice_build_audio(buf, sizeof(buf), 1, 1, 0, VOICE_CODEC_OPUS, frame, sizeof(frame)) == 0);
}

static void test_build_end_layout(void) {
    uint8_t buf[VOICE_MAX_PKT];
    size_t n = voice_build_end(buf, 0xFFFF, 0x0085, 0x01020304, VOICE_END_TIMEOUT);
    const uint8_t want[] = {0x01, 0x03, 0xFF, 0xFF, 0x00, 0x85,
                            0x01, 0x02, 0x03, 0x04, 0x01};
    assert(n == 11);
    assert(memcmp(buf, want, sizeof(want)) == 0);
}

static void test_audio_payload_strips_report_id(void) {
    const VoiceProfile *p = voice_profile_find("firetv");
    uint8_t report[81];
    report[0] = 0xF0;
    for (int i = 1; i < 81; i++) report[i] = (uint8_t)i;
    size_t len = 0;
    const uint8_t *payload = voice_audio_payload(p, report, sizeof(report), &len);
    assert(payload == report + 1);
    assert(len == 80);
}

static void test_audio_payload_ignores_other_report_ids(void) {
    const VoiceProfile *p = voice_profile_find("firetv");
    const uint8_t key_report[] = {0x02, 0x21, 0x02, 0x00, 0x00};
    size_t len = 99;
    assert(voice_audio_payload(p, key_report, sizeof(key_report), &len) == NULL);
    const uint8_t id_only[] = {0xF0};
    assert(voice_audio_payload(p, id_only, sizeof(id_only), &len) == NULL);
}

int main(void) {
    test_firetv_profile_values();
    test_unknown_profile_is_null();
    test_build_start_layout();
    test_build_audio_layout();
    test_build_audio_too_big_returns_0();
    test_build_end_layout();
    test_audio_payload_strips_report_id();
    test_audio_payload_ignores_other_report_ids();
    printf("all voice tests passed\n");
    return 0;
}
