/* Unit tests for packet.c (v3 wire encoding, Bluetooth address helpers).
 * Run with `make test`. */
#include <stdio.h>
#include <string.h>

#include "../packet.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_key_layout(void) {
    uint8_t k[PKT_KEY_LEN];
    CHECK(packet_build_key(k, 0x01020304, 0x0067, 0x000C0033, 1) == 13);
    const uint8_t want[] = {0x30, 1, 2, 3, 4, 0x01, 0x00, 0x67, 0x00, 0x0C, 0x00, 0x33, 0x01};
    CHECK(memcmp(k, want, sizeof(want)) == 0);
}

static void test_key_negative_value(void) {
    uint8_t k[PKT_KEY_LEN];
    CHECK(packet_build_key(k, 0, 1, 0, -1) == 13);
    CHECK(k[12] == 0xFF);
}

static void test_status_layout(void) {
    uint8_t s[PKT_STATUS_LEN];
    CHECK(packet_build_status(s, 0x01020304, STATUS_DISCONNECTED, 39, 8100) == 12);
    const uint8_t want[] = {0x30, 1, 2, 3, 4, 0x02, 0x02, 39, 0x00, 0x00, 0x1F, 0xA4};
    CHECK(memcmp(s, want, sizeof(want)) == 0);
}

static void test_bt_dev_suffix(void) {
    char out[32];
    CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0", out, sizeof out) == 0 && strcmp(out, "dev_18_BF_1C_D3_0B_A0") == 0);
    CHECK(bt_dev_suffix("18:BF:1C:D3:0B:A0", out, sizeof out) == 0 && strcmp(out, "dev_18_BF_1C_D3_0B_A0") == 0);
    CHECK(bt_dev_suffix("", out, sizeof out) == -1);
    CHECK(bt_dev_suffix("18:bf:1c:d3:0b", out, sizeof out) == -1);
    CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0:ff", out, sizeof out) == -1);
    CHECK(bt_dev_suffix("zz:bf:1c:d3:0b:a0", out, sizeof out) == -1);
    CHECK(bt_dev_suffix("usb-3f980000.usb-1.2", out, sizeof out) == -1);
    CHECK(bt_dev_suffix("18:bf:1c:d3:0b:a0", out, 10) == -1);
}

static void test_bt_path_has_suffix(void) {
    const char *a = "dev_18_BF_1C_D3_0B_A0";
    CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0", a) == 1);
    CHECK(bt_path_has_suffix("/org/bluez/hci1/dev_18_BF_1C_D3_0B_A0", a) == 1);
    CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0/service0012", a) == 0);
    CHECK(bt_path_has_suffix("/org/bluez/hci0/xdev_18_BF_1C_D3_0B_A0", a) == 0);
    CHECK(bt_path_has_suffix("/org/bluez/hci0/dev_11_22_33_44_55_66", a) == 0);
}

int main(void) {
    test_key_layout(); test_key_negative_value(); test_status_layout();
    test_bt_dev_suffix(); test_bt_path_has_suffix();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all packet tests passed\n");
    return 0;
}
