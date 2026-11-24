#ifndef PACKET_H
#define PACKET_H

#include <stddef.h>
#include <stdint.h>

/* -------- Key-port wire format, version 3 (big-endian) --------
 * Prefix (6 bytes): header u8 (upper nibble = version, lower nibble
 *                   reserved 0), timestamp_ms u32, type u8
 * KEY:    + key_code u16 + scan_code u32 + value i8          (13 bytes)
 * STATUS: + kind u8 + battery u8 + session_ms u32            (12 bytes)
 */
#define BRIDGE_PKT_VERSION 3
enum { PKT_MSG_KEY = 1, PKT_MSG_STATUS = 2 };
enum { STATUS_CONNECTED = 1, STATUS_DISCONNECTED = 2, STATUS_BATTERY = 3 };
#define BATTERY_UNKNOWN 0xFF
#define PKT_KEY_LEN    13
#define PKT_STATUS_LEN 12

/* Both return the number of bytes written (PKT_KEY_LEN / PKT_STATUS_LEN). */
size_t packet_build_key(uint8_t *buf, uint32_t ts, uint16_t key_code, uint32_t scan_code, int8_t value);
size_t packet_build_status(uint8_t *buf, uint32_t ts, uint8_t kind, uint8_t battery, uint32_t session_ms);

/* Turn a Bluetooth address as reported in an evdev device's uniq
 * ("18:bf:1c:d3:0b:a0") into the last element of its BlueZ object path
 * ("dev_18_BF_1C_D3_0B_A0"). Returns 0, or -1 (out untouched) if uniq is
 * not a 6-octet address or out is too small. */
int bt_dev_suffix(const char *uniq, char *out, size_t cap);

/* 1 if object path `path` ends in "/<suffix>", else 0. */
int bt_path_has_suffix(const char *path, const char *suffix);

#endif
