#define REMOTE_BRIDGE_TEST
#include "../remote_bridge.c"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_press(void) {
    uint8_t prev[3] = {0};
    const uint8_t rpt[] = {0xEF, 0xA2, 0x00, 0x00, 0x00};
    AppButtonEvent ev[6];
    int n = parse_app_button_report(rpt, sizeof(rpt), prev, ev);
    CHECK(n == 1);
    CHECK(ev[0].code == 0xA2 && ev[0].value == 1);
    CHECK(app_button_scan_code(ev[0].code) == 0xFF00A2);
}

static void test_release(void) {
    uint8_t prev[3] = {0xA2, 0, 0};
    const uint8_t rpt[] = {0xEF, 0x00, 0x00, 0x00, 0x00};
    AppButtonEvent ev[6];
    int n = parse_app_button_report(rpt, sizeof(rpt), prev, ev);
    CHECK(n == 1);
    CHECK(ev[0].code == 0xA2 && ev[0].value == 0);
    CHECK(prev[0] == 0);
}

static void test_repeat_ignored(void) {
    uint8_t prev[3] = {0};
    const uint8_t rpt[] = {0xEF, 0xA1, 0x00, 0x00};
    AppButtonEvent ev[6];
    CHECK(parse_app_button_report(rpt, sizeof(rpt), prev, ev) == 1);
    CHECK(parse_app_button_report(rpt, sizeof(rpt), prev, ev) == 0);
}

static void test_two_codes_held(void) {
    uint8_t prev[3] = {0};
    AppButtonEvent ev[6];
    const uint8_t a[] = {0xEF, 0xA2, 0x00, 0x00};
    const uint8_t ab[] = {0xEF, 0xA2, 0xA4, 0x00};
    const uint8_t b[] = {0xEF, 0xA4, 0x00, 0x00};
    CHECK(parse_app_button_report(a, sizeof(a), prev, ev) == 1);
    int n = parse_app_button_report(ab, sizeof(ab), prev, ev);
    CHECK(n == 1 && ev[0].code == 0xA4 && ev[0].value == 1);
    n = parse_app_button_report(b, sizeof(b), prev, ev);
    CHECK(n == 1 && ev[0].code == 0xA2 && ev[0].value == 0);
}

static void test_other_reports_ignored(void) {
    uint8_t prev[3] = {0xA2, 0, 0};
    AppButtonEvent ev[6];
    const uint8_t kbd[] = {0x01, 0x66, 0x00, 0x00};
    const uint8_t cc[] = {0x02, 0x23, 0x02, 0x00, 0x00};
    const uint8_t f0[] = {0xF0, 0xA4, 0xA4, 0xA4};
    const uint8_t shortrpt[] = {0xEF, 0xA4};
    CHECK(parse_app_button_report(kbd, sizeof(kbd), prev, ev) == 0);
    CHECK(parse_app_button_report(cc, sizeof(cc), prev, ev) == 0);
    CHECK(parse_app_button_report(f0, sizeof(f0), prev, ev) == 0);
    CHECK(parse_app_button_report(shortrpt, sizeof(shortrpt), prev, ev) == 0);
    CHECK(prev[0] == 0xA2 && prev[1] == 0 && prev[2] == 0);
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
    test_press(); test_release(); test_repeat_ignored();
    test_two_codes_held(); test_other_reports_ignored(); test_scan_codes();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all hidraw tests passed\n");
    return 0;
}
