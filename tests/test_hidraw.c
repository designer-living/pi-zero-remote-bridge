#define REMOTE_BRIDGE_TEST
#include "../remote_bridge.c"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Report descriptor of the bedroom Fire TV remote (0171:0418): keyboard (0x01),
 * consumer (0x02), page 0x00FF voice reports (0xF0-0xF3) and button array
 * (0xEF), battery (0x03). */
static const uint8_t fire_tv_desc[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x85,0x01,0x95,0x03,0x75,0x08,0x15,0x00,
    0x25,0xff,0x19,0x00,0x29,0xff,0x81,0x00,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,
    0x02,0x95,0x02,0x75,0x10,0x15,0x00,0x26,0x9c,0x02,0x19,0x00,0x2a,0x9c,0x02,0x81,
    0x00,0xc0,0x06,0xff,0x00,0x09,0x00,0xa1,0x01,0x85,0xf0,0x95,0x50,0x75,0x08,0x15,
    0x00,0x25,0xff,0x81,0x00,0x85,0xf1,0x95,0x03,0x75,0x08,0x09,0x00,0x81,0x02,0x85,
    0xf2,0x95,0x01,0x75,0x08,0x09,0x00,0x91,0x02,0x85,0xf3,0x95,0x0a,0x75,0x08,0x09,
    0x00,0x91,0x02,0x85,0xef,0x95,0x03,0x75,0x08,0x15,0x00,0x25,0xff,0x19,0x00,0x29,
    0xff,0x09,0x01,0x81,0x00,0xc0,0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x03,0x05,0x01,
    0x09,0x06,0xa1,0x02,0x05,0x06,0x09,0x20,0x15,0x00,0x26,0x64,0x00,0x75,0x08,0x95,
    0x01,0x81,0x02,0xc0,0xc0,
};

static const HidButtonReport fire_tv = { .report_id = 0xEF, .offset = 1, .count = 3 };

static void test_descriptor_fire_tv(void) {
    HidButtonReport r;
    CHECK(hid_find_button_report(fire_tv_desc, sizeof(fire_tv_desc), &r) == 0);
    CHECK(r.report_id == 0xEF && r.offset == 1 && r.count == 3);
}

static void test_descriptor_without_vendor_buttons(void) {
    HidButtonReport r;
    /* keyboard + consumer collections only (first 50 bytes) */
    CHECK(hid_find_button_report(fire_tv_desc, 50, &r) == -1);
    /* array without a usage range (the 0xF0 voice audio report) */
    const uint8_t audio_only[] = {
        0x06,0xff,0x00,0x09,0x00,0xa1,0x01,0x85,0xf0,0x95,0x50,0x75,0x08,
        0x15,0x00,0x25,0xff,0x81,0x00,0xc0,
    };
    CHECK(hid_find_button_report(audio_only, sizeof(audio_only), &r) == -1);
    /* buttons declared as variables (bitmap), not an array */
    const uint8_t bitmap[] = {
        0x06,0x00,0xff,0xa1,0x01,0x85,0x05,0x95,0x08,0x75,0x01,
        0x19,0x01,0x29,0x08,0x81,0x02,0xc0,
    };
    CHECK(hid_find_button_report(bitmap, sizeof(bitmap), &r) == -1);
}

static void test_descriptor_other_layouts(void) {
    HidButtonReport r;
    /* no report ids, vendor page 0xFF01, 2 codes */
    const uint8_t no_ids[] = {
        0x06,0x01,0xff,0x09,0x01,0xa1,0x01,0x95,0x02,0x75,0x08,
        0x15,0x00,0x26,0xff,0x00,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,0xc0,
    };
    CHECK(hid_find_button_report(no_ids, sizeof(no_ids), &r) == 0);
    CHECK(r.report_id == 0 && r.offset == 0 && r.count == 2);
    /* array preceded by a 1-byte variable in the same report */
    const uint8_t after_var[] = {
        0x06,0x00,0xff,0xa1,0x01,0x85,0x07,0x95,0x01,0x75,0x08,0x09,0x00,0x81,0x02,
        0x95,0x04,0x19,0x00,0x29,0xff,0x81,0x00,0xc0,
    };
    CHECK(hid_find_button_report(after_var, sizeof(after_var), &r) == 0);
    CHECK(r.report_id == 0x07 && r.offset == 2 && r.count == 4);
    /* keyboard-page array: the kernel already turns these into keys */
    const uint8_t keyboard[] = {
        0x05,0x07,0xa1,0x01,0x85,0x01,0x95,0x03,0x75,0x08,0x19,0x00,0x29,0xff,0x81,0x00,0xc0,
    };
    CHECK(hid_find_button_report(keyboard, sizeof(keyboard), &r) == -1);
    /* truncated item must not read past the end */
    const uint8_t truncated[] = { 0x06,0x00 };
    CHECK(hid_find_button_report(truncated, sizeof(truncated), &r) == -1);
}

static void test_press(void) {
    uint8_t prev[HID_APP_CODES_MAX] = {0};
    const uint8_t rpt[] = {0xEF, 0xA2, 0x00, 0x00, 0x00};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    int n = parse_app_button_report(&fire_tv, rpt, sizeof(rpt), prev, ev);
    CHECK(n == 1);
    CHECK(ev[0].code == 0xA2 && ev[0].value == 1);
    CHECK(app_button_scan_code(ev[0].code) == 0xFF00A2);
}

static void test_release(void) {
    uint8_t prev[HID_APP_CODES_MAX] = {0xA2, 0, 0};
    const uint8_t rpt[] = {0xEF, 0x00, 0x00, 0x00, 0x00};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    int n = parse_app_button_report(&fire_tv, rpt, sizeof(rpt), prev, ev);
    CHECK(n == 1);
    CHECK(ev[0].code == 0xA2 && ev[0].value == 0);
    CHECK(prev[0] == 0);
}

static void test_repeat_ignored(void) {
    uint8_t prev[HID_APP_CODES_MAX] = {0};
    const uint8_t rpt[] = {0xEF, 0xA1, 0x00, 0x00};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    CHECK(parse_app_button_report(&fire_tv, rpt, sizeof(rpt), prev, ev) == 1);
    CHECK(parse_app_button_report(&fire_tv, rpt, sizeof(rpt), prev, ev) == 0);
}

static void test_two_codes_held(void) {
    uint8_t prev[HID_APP_CODES_MAX] = {0};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    const uint8_t a[] = {0xEF, 0xA2, 0x00, 0x00};
    const uint8_t ab[] = {0xEF, 0xA2, 0xA4, 0x00};
    const uint8_t b[] = {0xEF, 0xA4, 0x00, 0x00};
    CHECK(parse_app_button_report(&fire_tv, a, sizeof(a), prev, ev) == 1);
    int n = parse_app_button_report(&fire_tv, ab, sizeof(ab), prev, ev);
    CHECK(n == 1 && ev[0].code == 0xA4 && ev[0].value == 1);
    n = parse_app_button_report(&fire_tv, b, sizeof(b), prev, ev);
    CHECK(n == 1 && ev[0].code == 0xA2 && ev[0].value == 0);
}

static void test_other_reports_ignored(void) {
    uint8_t prev[HID_APP_CODES_MAX] = {0xA2, 0, 0};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    const uint8_t kbd[] = {0x01, 0x66, 0x00, 0x00};
    const uint8_t cc[] = {0x02, 0x23, 0x02, 0x00, 0x00};
    const uint8_t f0[] = {0xF0, 0xA4, 0xA4, 0xA4};
    const uint8_t shortrpt[] = {0xEF, 0xA4};
    CHECK(parse_app_button_report(&fire_tv, kbd, sizeof(kbd), prev, ev) == 0);
    CHECK(parse_app_button_report(&fire_tv, cc, sizeof(cc), prev, ev) == 0);
    CHECK(parse_app_button_report(&fire_tv, f0, sizeof(f0), prev, ev) == 0);
    CHECK(parse_app_button_report(&fire_tv, shortrpt, sizeof(shortrpt), prev, ev) == 0);
    CHECK(prev[0] == 0xA2 && prev[1] == 0 && prev[2] == 0);
}

static void test_no_report_id(void) {
    const HidButtonReport r = { .report_id = 0, .offset = 0, .count = 2 };
    uint8_t prev[HID_APP_CODES_MAX] = {0};
    AppButtonEvent ev[2 * HID_APP_CODES_MAX];
    const uint8_t rpt[] = {0x00, 0x42};
    int n = parse_app_button_report(&r, rpt, sizeof(rpt), prev, ev);
    CHECK(n == 1 && ev[0].code == 0x42 && ev[0].value == 1);
}

static void test_scan_codes(void) {
    CHECK(app_button_scan_code(0xA1) == 0xFF00A1);
    CHECK(app_button_scan_code(0xA2) == 0xFF00A2);
    CHECK(app_button_scan_code(0xA3) == 0xFF00A3);
    CHECK(app_button_scan_code(0xA4) == 0xFF00A4);
    CHECK(strcmp(app_button_name(0xA2), "Netflix") == 0);
    CHECK(strcmp(app_button_name(0xA1), "Amazon Video") == 0);
    CHECK(strcmp(app_button_name(0xA3), "App Button") == 0);
    CHECK(strcmp(app_button_name(0xA4), "Amazon Music") == 0);
    CHECK(strcmp(app_button_name(0x77), "unknown") == 0);
}

int main(void) {
    test_descriptor_fire_tv(); test_descriptor_without_vendor_buttons(); test_descriptor_other_layouts();
    test_press(); test_release(); test_repeat_ignored();
    test_two_codes_held(); test_other_reports_ignored(); test_no_report_id(); test_scan_codes();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all hidraw tests passed\n");
    return 0;
}
