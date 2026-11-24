#include "packet.h"

#include <ctype.h>
#include <string.h>

static size_t put_prefix(uint8_t *buf, uint32_t ts, uint8_t type) {
    buf[0] = (uint8_t)(BRIDGE_PKT_VERSION << 4);
    buf[1] = (uint8_t)(ts >> 24);
    buf[2] = (uint8_t)(ts >> 16);
    buf[3] = (uint8_t)(ts >> 8);
    buf[4] = (uint8_t)ts;
    buf[5] = type;
    return 6;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

size_t packet_build_key(uint8_t *buf, uint32_t ts, uint16_t key_code, uint32_t scan_code, int8_t value) {
    size_t n = put_prefix(buf, ts, PKT_MSG_KEY);
    buf[n++] = (uint8_t)(key_code >> 8);
    buf[n++] = (uint8_t)key_code;
    put_u32(buf + n, scan_code);
    n += 4;
    buf[n++] = (uint8_t)value;
    return n;
}

size_t packet_build_status(uint8_t *buf, uint32_t ts, uint8_t kind, uint8_t battery, uint32_t session_ms) {
    size_t n = put_prefix(buf, ts, PKT_MSG_STATUS);
    buf[n++] = kind;
    buf[n++] = battery;
    put_u32(buf + n, session_ms);
    n += 4;
    return n;
}

int bt_dev_suffix(const char *uniq, char *out, size_t cap) {
    /* "xx:xx:xx:xx:xx:xx" -> "dev_XX_XX_XX_XX_XX_XX" (21 chars + NUL) */
    if (!uniq || strlen(uniq) != 17 || cap < 22) return -1;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (uniq[i] != ':') return -1;
        } else if (!isxdigit((unsigned char)uniq[i])) {
            return -1;
        }
    }
    memcpy(out, "dev_", 4);
    for (int i = 0; i < 17; i++) {
        out[4 + i] = (i % 3 == 2) ? '_' : (char)toupper((unsigned char)uniq[i]);
    }
    out[21] = '\0';
    return 0;
}

int bt_path_has_suffix(const char *path, const char *suffix) {
    size_t lp = strlen(path), ls = strlen(suffix);
    if (ls == 0 || lp < ls + 1) return 0;
    return path[lp - ls - 1] == '/' && strcmp(path + lp - ls, suffix) == 0;
}
