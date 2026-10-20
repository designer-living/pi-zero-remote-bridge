#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/inotify.h>
#else
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#define EV_KEY 0x01
#define EV_MSC 0x04
#define MSC_SCAN 0x04
#define IN_NONBLOCK 0x00000800
#define IN_CREATE 0x00000100
#define IN_ATTRIB 0x00000004
#define IN_DELETE 0x00000200
#define IN_MOVED_TO 0x00000080
struct inotify_event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};
static int inotify_init1(int flags) { (void)flags; return -1; }
static int inotify_add_watch(int fd, const char *pathname, uint32_t mask) { (void)fd; (void)pathname; (void)mask; return -1; }
#define EVIOCGNAME(len) 0
#define EVIOCGPHYS(len) 0
#define EVIOCGUNIQ(len) 0
#endif
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>
#include "voice.h"
#include <stdarg.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define INPUT_DIR "/dev/input"
#define MAX_NAME 256

#define PACKET_FORMAT_VERSION    2
#define PKT_MAKE_HEADER(ver)     ((uint8_t)(((ver) & 0x0F) << 4))
#define PKT_HEADER_VERSION(h)    (((h) >> 4) & 0x0F)

/* -------- Binary packet -------- */
struct Packet {
    uint8_t  header;        /* upper 4 bits = format version (0-15), lower 4 bits reserved */
    uint32_t timestamp_ms;  /* network byte order */
    uint16_t key_code;      /* network byte order */
    uint32_t scan_code;     /* network byte order; raw MSC_SCAN value preceding this key, 0 if none */
    int8_t   value;         /* 0=up, 1=down, 2=repeat */
} __attribute__((packed));

/* -------- Time helper -------- */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           ts.tv_nsec / 1000000ULL;
}

enum {
    LEVEL_ALWAYS = -1,
    LEVEL_ERROR = 0,
    LEVEL_INFO,
    LEVEL_DEBUG,
    LEVEL_TRACE
};

static int log_level = LEVEL_INFO;
static int edge_repeat_count = 1;

/* -------- Logging helper with UTC timestamp -------- */
static void log_print(int level, const char *level_name, const char *fmt, ...) {
    if (level != LEVEL_ALWAYS && level > log_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);
    char tstr[64];
    int len = strftime(tstr, sizeof(tstr), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    snprintf(tstr + len, sizeof(tstr) - len, ".%03ldZ", ts.tv_nsec / 1000000L);

    if (level == LEVEL_ALWAYS) {
        printf("[%s] ", tstr);
    } else {
        printf("[%s] [%s] ", tstr, level_name);
    }
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

#define LOG_ALWAYS(fmt, ...) log_print(LEVEL_ALWAYS, "ALWAYS", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_print(LEVEL_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_print(LEVEL_INFO,  "INFO",  fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_print(LEVEL_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) log_print(LEVEL_TRACE, "TRACE", fmt, ##__VA_ARGS__)

/* Format len bytes at buf as "XX XX XX ..." into out (must be >= len*3+1 bytes) */
static void hex_bytes(const void *buf, size_t len, char *out) {
    if (!out) return;
    if (!buf || len == 0) {
        out[0] = '\0';
        return;
    }
    const uint8_t *b = (const uint8_t *)buf;
    static const char hex_chars[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        size_t off = i * 3;
        out[off]     = hex_chars[b[i] >> 4];
        out[off + 1] = hex_chars[b[i] & 0x0F];
        out[off + 2] = ' ';
    }
    out[len * 3 - 1] = '\0';
}

static const char* level_to_str(int level) {
    switch (level) {
        case LEVEL_ALWAYS: return "ALWAYS";
        case LEVEL_ERROR:  return "ERROR";
        case LEVEL_INFO:   return "INFO";
        case LEVEL_DEBUG:  return "DEBUG";
        case LEVEL_TRACE:  return "TRACE";
        default:           return "UNKNOWN";
    }
}

#define MAX_MAPPINGS 32

/* Fire TV remote ("AR", Bluetooth HID): its Netflix / Amazon Video / App /
 * Amazon Music buttons never reach evdev. They only arrive as vendor-page
 * (0xFF00) input report 0xEF on the hidraw node: <0xEF> <code> <code> <code>,
 * a keyboard-style array of up to 3 held codes, all zero when released. */
#define HID_VENDOR_ID           0x0171
#define HID_PRODUCT_ID          0x041E
#define HID_APP_REPORT_ID       0xEF
#define HID_APP_CODES           3
#define HID_APP_SCAN_BASE       0xFF0000u   /* scan code = base | code; 0xFF00 page can't collide with consumer-page 0xc00xx */
#define KEY_UNKNOWN_CODE        240
#define HIDRAW_SYSFS_DIR        "/sys/class/hidraw"

typedef struct {
    uint8_t code;
    int value;              /* 1 = press, 0 = release */
} AppButtonEvent;

static uint32_t app_button_scan_code(uint8_t code) {
    return HID_APP_SCAN_BASE | code;
}

static const char *app_button_name(uint8_t code) {
    switch (code) {
        case 0xA1: return "Amazon Video";
        case 0xA2: return "Netflix";
        case 0xA3: return "App Button";
        case 0xA4: return "Amazon Music";
        default:   return "unknown";
    }
}

/* Diffs a 0xEF report against the previous array (updated in place).
 * Returns the number of events written to out (max 2 * HID_APP_CODES),
 * releases first. Reports with any other ID, or too short, are ignored. */
static int parse_app_button_report(const uint8_t *buf, size_t len,
                                   uint8_t prev[HID_APP_CODES], AppButtonEvent *out) {
    if (len < 1 + HID_APP_CODES || buf[0] != HID_APP_REPORT_ID) return 0;

    const uint8_t *cur = buf + 1;
    int n = 0;
    for (int i = 0; i < HID_APP_CODES; i++) {
        if (prev[i] == 0) continue;
        int still_held = 0;
        for (int j = 0; j < HID_APP_CODES; j++) if (cur[j] == prev[i]) still_held = 1;
        if (!still_held) out[n++] = (AppButtonEvent){ prev[i], 0 };
    }
    for (int i = 0; i < HID_APP_CODES; i++) {
        if (cur[i] == 0) continue;
        int seen = 0;
        for (int j = 0; j < HID_APP_CODES; j++) if (prev[j] == cur[i]) seen = 1;
        for (int j = 0; j < i; j++) if (cur[j] == cur[i]) seen = 1;
        if (!seen) out[n++] = (AppButtonEvent){ cur[i], 1 };
    }
    memcpy(prev, cur, HID_APP_CODES);
    return n;
}

typedef struct {
    char name[MAX_NAME];
    char id_match[64];      /* optional substring of the device's EVIOCGPHYS location or EVIOCGUNIQ id; "" = match any device */
    char dev_path[256];
    struct sockaddr_in server_addr;
    int evfd;
    uint32_t pending_scan_code;
    char uniq[64];          /* EVIOCGUNIQ of the matched evdev node; ties the remote's hidraw node to this mapping */
    int hidfd;              /* hidraw node of the same remote (vendor app buttons), or -1 */
    char hid_path[256];
    uint8_t hid_prev[HID_APP_CODES];
    /* Voice capture (only when a VOICE= line names this remote) */
    const VoiceProfile *voice;      /* NULL = voice off: trigger key is a normal key */
    struct sockaddr_in voice_addr;  /* server IP with the voice port */
    int voice_hidfd;                      /* the remote's /dev/hidrawN, -1 if not open */
    int mic_open;
    uint16_t sid;
    uint16_t seq;
    uint64_t mic_opened_ms;
    uint32_t frames;
} Mapping;

static Mapping mappings[MAX_MAPPINGS];
static int num_mappings = 0;

static int add_mapping(const char *name, const char *ip, int port, const char *id_match) {
    if (num_mappings >= MAX_MAPPINGS) {
        LOG_ERROR("Maximum number of mappings (%d) reached\n", MAX_MAPPINGS);
        return -1;
    }
    Mapping *m = &mappings[num_mappings++];
    strncpy(m->name, name, MAX_NAME - 1);
    m->name[MAX_NAME - 1] = '\0';
    if (id_match && *id_match) {
        strncpy(m->id_match, id_match, sizeof(m->id_match) - 1);
        m->id_match[sizeof(m->id_match) - 1] = '\0';
    } else {
        m->id_match[0] = '\0';
    }
    m->dev_path[0] = '\0';
    m->evfd = -1;
    m->pending_scan_code = 0;
    m->uniq[0] = '\0';
    m->hidfd = -1;
    m->hid_path[0] = '\0';
    memset(m->hid_prev, 0, sizeof(m->hid_prev));
    m->voice = NULL;
    m->voice_hidfd = -1;
    m->mic_open = 0;

    memset(&m->server_addr, 0, sizeof(m->server_addr));
    m->server_addr.sin_family = AF_INET;
    m->server_addr.sin_port   = htons(port);
    if (!inet_aton(ip, &m->server_addr.sin_addr)) {
        LOG_ERROR("Invalid IP address: %s\n", ip);
        num_mappings--;
        return -1;
    }
    return 0;
}

static int find_available_mapping(int fd, const char *path) {
    char name[MAX_NAME] = {0};
    char phys[64] = {0};
    char uniq[64] = {0};

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        LOG_DEBUG("Failed to get name for %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys) < 0) {
        phys[0] = '\0';   /* USB: bus/port path; not all devices report it */
    }
    if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq) < 0) {
        uniq[0] = '\0';   /* Bluetooth: the remote's own address; empty on most USB devices */
    }

    LOG_DEBUG("Checking device %s: name=\"%s\" phys=\"%s\" uniq=\"%s\"\n", path, name, phys, uniq);

    /* Two passes: mappings with an id filter first, so two identically-named
     * remotes each land on their own line; then mappings with no filter,
     * which match any device. The filter is a substring of either the
     * device's physical location (USB port path) or its unique id
     * (Bluetooth address). */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < num_mappings; i++) {
            if (mappings[i].evfd >= 0) continue;

            int has_filter = mappings[i].id_match[0] != '\0';
            if ((pass == 0) != (has_filter != 0)) continue;

            if (strcmp(mappings[i].name, name) != 0) {
                LOG_TRACE("Mapping %d name (\"%s\") does not match device name (\"%s\")\n",
                    i, mappings[i].name, name);
                continue;
            }

            if (has_filter &&
                (phys[0] == '\0' || strstr(phys, mappings[i].id_match) == NULL) &&
                (uniq[0] == '\0' || strstr(uniq, mappings[i].id_match) == NULL)) {
                LOG_TRACE("Mapping %d id filter (\"%s\") matches neither phys (\"%s\") nor uniq (\"%s\")\n",
                    i, mappings[i].id_match, phys, uniq);
                continue;
            }

            snprintf(mappings[i].uniq, sizeof(mappings[i].uniq), "%s", uniq);
            LOG_INFO("Matched device: %s at %s (phys \"%s\" uniq \"%s\") for mapping %d\n",
                name, path, phys, uniq, i);
            return i;
        }
    }

    return -1;
}

static void send_key_packet(int sock, Mapping *m, uint16_t key_code, uint32_t scan_code, int value) {
    struct Packet pkt;
    uint32_t event_timestamp = (uint32_t)(now_ms() & 0xFFFFFFFF);

    int send_count = (value == 0 || value == 1) ? edge_repeat_count : 1;

    for (int repeat = 0; repeat < send_count; repeat++) {
        pkt.header       = PKT_MAKE_HEADER(PACKET_FORMAT_VERSION);
        pkt.timestamp_ms = htonl(event_timestamp);
        pkt.key_code     = htons(key_code);
        pkt.scan_code    = htonl(scan_code);
        pkt.value        = (int8_t)value;

        if (log_level >= LEVEL_TRACE) {
            char hex[sizeof(pkt) * 3 + 1];
            hex_bytes(&pkt, sizeof(pkt), hex);
            LOG_TRACE("Sending packet (%zu bytes): buf=[%s]\n",
                sizeof(pkt), hex);
        }
        ssize_t sent = sendto(sock,
                              &pkt, sizeof(pkt), 0,
                              (struct sockaddr *)&m->server_addr,
                              sizeof(struct sockaddr_in));
        if (sent < 0) {
            LOG_ERROR("Failed to send UDP packet: %s\n", strerror(errno));
        } else {
            LOG_TRACE("Sent UDP packet for key %d, value %d to %s:%d\n",
                key_code, value,
                inet_ntoa(m->server_addr.sin_addr),
                ntohs(m->server_addr.sin_port));
        }
    }
}

static void close_hidraw(Mapping *m) {
    if (m->hidfd >= 0) close(m->hidfd);
    m->hidfd = -1;
    m->hid_path[0] = '\0';
    memset(m->hid_prev, 0, sizeof(m->hid_prev));
}

/* Finds a hidraw node belonging to the Fire TV remote with the given
 * Bluetooth address (HID_UNIQ in sysfs), by vendor/product id, not by number. */
static int find_hidraw_path(const char *uniq, char *path, size_t path_len) {
    DIR *d = opendir(HIDRAW_SYSFS_DIR);
    if (!d) return -1;

    struct dirent *ent;
    int found = -1;
    while (found < 0 && (ent = readdir(d))) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;

        char uevent[sizeof(HIDRAW_SYSFS_DIR) + 2 * 256 + 16];
        snprintf(uevent, sizeof(uevent), HIDRAW_SYSFS_DIR "/%s/device/uevent", ent->d_name);
        FILE *f = fopen(uevent, "r");
        if (!f) continue;

        unsigned vid = 0, pid = 0;
        char hid_uniq[64] = {0};
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned bus;
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) continue;
            sscanf(line, "HID_UNIQ=%63s", hid_uniq);
        }
        fclose(f);

        if (vid != HID_VENDOR_ID || pid != HID_PRODUCT_ID) continue;
        if (uniq[0] == '\0' || strcasecmp(uniq, hid_uniq) != 0) continue;

        char candidate[256];
        snprintf(candidate, sizeof(candidate), "/dev/%.200s", ent->d_name);
        int in_use = 0;
        for (int i = 0; i < num_mappings; i++)
            if (strcmp(mappings[i].hid_path, candidate) == 0) in_use = 1;
        if (in_use) continue;

        snprintf(path, path_len, "%s", candidate);
        found = 0;
    }
    closedir(d);
    return found;
}

/* Opens the hidraw node for every matched remote that doesn't have one yet.
 * Safe to call repeatedly (after an evdev match, or when a hidraw node appears). */
static void attach_hidraw(void) {
    for (int i = 0; i < num_mappings; i++) {
        Mapping *m = &mappings[i];
        if (m->evfd < 0 || m->hidfd >= 0) continue;

        char path[256];
        if (find_hidraw_path(m->uniq, path, sizeof(path)) < 0) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            LOG_DEBUG("Failed to open %s for \"%s\": %s\n", path, m->name, strerror(errno));
            continue;
        }
        m->hidfd = fd;
        snprintf(m->hid_path, sizeof(m->hid_path), "%s", path);
        memset(m->hid_prev, 0, sizeof(m->hid_prev));
        LOG_INFO("Opened %s for app buttons of \"%s\"\n", path, m->name);
    }
}

static void handle_hidraw(int sock, Mapping *m) {
    uint8_t buf[256];
    while (1) {
        ssize_t n = read(m->hidfd, buf, sizeof(buf));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) {
            LOG_INFO("hidraw %s for \"%s\" closed (%s)\n", m->hid_path, m->name,
                     n == 0 ? "EOF" : strerror(errno));
            close_hidraw(m);
            break;
        }

        AppButtonEvent evs[2 * HID_APP_CODES];
        int count = parse_app_button_report(buf, (size_t)n, m->hid_prev, evs);
        for (int i = 0; i < count; i++) {
            uint32_t scan = app_button_scan_code(evs[i].code);
            LOG_TRACE("App Button Event (%s): %s code=0x%02x scan_code=0x%x value=%d\n",
                      m->name, app_button_name(evs[i].code), evs[i].code, (unsigned)scan, evs[i].value);
            send_key_packet(sock, m, KEY_UNKNOWN_CODE, scan, evs[i].value);
        }
    }
}

/* VOICE=<exact_remote_name>,<profile>,<voice_port>: applies to every mapping
 * with that name; voice packets go to the mapping's server IP on voice_port. */
static void apply_voice_line(const char *val) {
    char name[MAX_NAME], profile[64];
    int port;
    if (sscanf(val, " %255[^,] , %63[^, ] , %d", name, profile, &port) != 3 || port <= 0 || port > 65535) {
        LOG_ERROR("Invalid VOICE line (want VOICE=<remote name>,<profile>,<voice port>): %s\n", val);
        return;
    }
    size_t n_len = strlen(name);
    while (n_len > 0 && (name[n_len - 1] == ' ' || name[n_len - 1] == '\t')) name[--n_len] = '\0';
    const VoiceProfile *vp = voice_profile_find(profile);
    if (!vp) {
        LOG_ERROR("Unknown voice profile \"%s\" in VOICE line: %s\n", profile, val);
        return;
    }
    int matched = 0;
    for (int i = 0; i < num_mappings; i++) {
        if (strcmp(mappings[i].name, name) != 0) continue;
        mappings[i].voice = vp;
        mappings[i].voice_addr = mappings[i].server_addr;
        mappings[i].voice_addr.sin_port = htons((uint16_t)port);
        matched++;
        LOG_DEBUG("Voice enabled: Remote=\"%s\", profile=%s, voice port=%d\n", name, vp->name, port);
    }
    if (!matched) LOG_ERROR("VOICE line names no configured REMOTE: %s\n", val);
}

static int load_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        LOG_ERROR("Failed to open config file %s: %s\n", filename, strerror(errno));
        return -1;
    }

    char line[512];
    /* VOICE= lines are applied after every REMOTE= line, so order in the file doesn't matter. */
    static char voice_lines[MAX_MAPPINGS][512];
    int num_voice_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        // Strip trailing newline
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) {
            p[len - 1] = '\0';
            len--;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        // Trim trailing spaces from key
        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
            key[--klen] = '\0';
        }

        // Trim leading and trailing spaces from val
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
            val[--vlen] = '\0';
        }

        // Trim quotes from val if present
        if (*val == '"') {
            val++;
            char *last_quote = strrchr(val, '"');
            if (last_quote) *last_quote = '\0';
        }

        if (strcasecmp(key, "LOG_LEVEL") == 0) {
            if (strcasecmp(val, "TRACE") == 0) log_level = LEVEL_TRACE;
            else if (strcasecmp(val, "DEBUG") == 0) log_level = LEVEL_DEBUG;
            else if (strcasecmp(val, "INFO") == 0)  log_level = LEVEL_INFO;
            else if (strcasecmp(val, "ERROR") == 0) log_level = LEVEL_ERROR;
        } else if (strcasecmp(key, "EDGE_REPEAT_COUNT") == 0) {
            edge_repeat_count = atoi(val);
            if (edge_repeat_count < 1) edge_repeat_count = 1;
        } else if (strcasecmp(key, "VOICE") == 0) {
            if (num_voice_lines < MAX_MAPPINGS) {
                strncpy(voice_lines[num_voice_lines], val, sizeof(voice_lines[0]) - 1);
                voice_lines[num_voice_lines][sizeof(voice_lines[0]) - 1] = '\0';
                num_voice_lines++;
            } else {
                LOG_ERROR("Too many VOICE lines (max %d)\n", MAX_MAPPINGS);
            }
        } else if (strcasecmp(key, "REMOTE") == 0) {
            char name[MAX_NAME], ip[64], id[64] = {0};
            int port;
            // Skip leading/trailing spaces in the sscanf fields. The 4th
            // field (device id substring: USB port path or Bluetooth
            // address) is optional.
            int n = sscanf(val, " %255[^,] , %63[^,] , %d , %63[^,]", name, ip, &port, id);
            if (n >= 3) {
                // Trim trailing spaces from name
                size_t n_len = strlen(name);
                while (n_len > 0 && (name[n_len - 1] == ' ' || name[n_len - 1] == '\t')) {
                    name[n_len - 1] = '\0';
                    n_len--;
                }
                // Trim trailing spaces from ip
                size_t i_len = strlen(ip);
                while (i_len > 0 && (ip[i_len - 1] == ' ' || ip[i_len - 1] == '\t')) {
                    ip[i_len - 1] = '\0';
                    i_len--;
                }
                // Trim leading and trailing spaces from the optional id field
                char *id_p = id;
                while (*id_p == ' ' || *id_p == '\t') id_p++;
                size_t p_len = strlen(id_p);
                while (p_len > 0 && (id_p[p_len - 1] == ' ' || id_p[p_len - 1] == '\t')) {
                    id_p[--p_len] = '\0';
                }

                if (add_mapping(name, ip, port, n >= 4 ? id_p : NULL) == 0) {
                    if (n >= 4 && *id_p) {
                        LOG_DEBUG("Loaded mapping: Remote=\"%s\", Server=%s:%d, Id~=\"%s\"\n",
                            name, ip, port, id_p);
                    } else {
                        LOG_DEBUG("Loaded mapping: Remote=\"%s\", Server=%s:%d\n",
                            name, ip, port);
                    }
                }
            } else {
                LOG_ERROR("Invalid REMOTE line: %s=%s\n", key, val);
            }
        }
    }
    fclose(f);
    for (int i = 0; i < num_voice_lines; i++) apply_voice_line(voice_lines[i]);
    return 0;
}

#ifndef REMOTE_BRIDGE_TEST

/* Open the hidraw node of a voice-enabled mapping's matched evdev device.
 * Failure leaves voice_hidfd = -1, and the trigger key then behaves as a normal key. */
static void voice_open_hidraw(Mapping *m) {
    if (!m->voice || m->voice_hidfd >= 0 || m->dev_path[0] == '\0') return;
    const char *event_name = strrchr(m->dev_path, '/');
    event_name = event_name ? event_name + 1 : m->dev_path;
    char hid_path[64];
    if (voice_find_hidraw_for_event("/sys", event_name, hid_path, sizeof(hid_path)) != 0) {
        LOG_ERROR("Voice: no hidraw node found for \"%s\" (%s); voice button will be sent as a normal key\n",
            m->name, m->dev_path);
        return;
    }
    m->voice_hidfd = open(hid_path, O_RDWR | O_NONBLOCK);
    if (m->voice_hidfd < 0) {
        LOG_ERROR("Voice: cannot open %s for \"%s\": %s (check hidraw permissions); voice button will be sent as a normal key\n",
            hid_path, m->name, strerror(errno));
        return;
    }
    LOG_INFO("Voice: \"%s\" using %s (profile %s)\n", m->name, hid_path, m->voice->name);
}

/* -------- Voice capture (main-loop helpers) -------- */
#define VOICE_MIC_CAP_MS 15000

static uint16_t next_sid;

static void voice_send(Mapping *m, int sock, const uint8_t *buf, size_t len, int repeats) {
    for (int r = 0; r < repeats; r++) {
        if (log_level >= LEVEL_TRACE) {
            char hex[VOICE_MAX_PKT * 3 + 1];
            hex_bytes(buf, len, hex);
            LOG_TRACE("Sending voice packet (%zu bytes): buf=[%s]\n", len, hex);
        }
        if (sendto(sock, buf, len, 0, (struct sockaddr *)&m->voice_addr, sizeof(m->voice_addr)) < 0) {
            LOG_ERROR("Failed to send voice packet: %s\n", strerror(errno));
        }
    }
}

static void voice_hid_write(Mapping *m, const uint8_t *report, size_t len) {
    if (m->voice_hidfd < 0) return;
    if (write(m->voice_hidfd, report, len) != (ssize_t)len) {
        LOG_ERROR("Voice: failed to write mic report to \"%s\": %s\n", m->name, strerror(errno));
    }
}

/* Switch the mic off (if open) and send END `reason`. */
static void voice_mic_close(Mapping *m, int sock, uint8_t reason) {
    if (!m->mic_open) return;
    voice_hid_write(m, m->voice->mic_off, m->voice->mic_off_len);
    uint8_t buf[VOICE_MAX_PKT];
    size_t len = voice_build_end(buf, m->sid, (uint16_t)(m->seq + 1), (uint32_t)(now_ms() & 0xFFFFFFFF), reason);
    voice_send(m, sock, buf, len, edge_repeat_count);
    m->mic_open = 0;
    LOG_INFO("Voice end (%s): sid=%u frames=%u reason=%u\n", m->name, m->sid, m->frames, reason);
}

/* Remote gone: end any open session and drop the hidraw fd. */
static void voice_close(Mapping *m, int sock, uint8_t reason) {
    voice_mic_close(m, sock, reason);
    if (m->voice_hidfd >= 0) {
        close(m->voice_hidfd);
        m->voice_hidfd = -1;
    }
}

/* Handle the voice trigger key. Returns 1 if consumed (no key packet sent). */
static int voice_on_key(Mapping *m, int value, int sock) {
    if (!m->voice) return 0;
    if (m->voice_hidfd < 0) voice_open_hidraw(m);   /* hidraw may appear after evdev */
    if (m->voice_hidfd < 0) return 0;               /* fall back to a normal key */

    if (value == 1) {
        if (m->mic_open) return 1;
        voice_hid_write(m, m->voice->mic_on, m->voice->mic_on_len);
        m->mic_open = 1;
        m->seq = 0;
        m->frames = 0;
        m->sid = ++next_sid;
        m->mic_opened_ms = now_ms();
        uint8_t buf[VOICE_MAX_PKT];
        size_t len = voice_build_start(buf, m->sid, (uint32_t)(m->mic_opened_ms & 0xFFFFFFFF),
                                       m->voice->codec, m->voice->trigger_key);
        voice_send(m, sock, buf, len, edge_repeat_count);
        LOG_INFO("Voice start (%s): sid=%u\n", m->name, m->sid);
    } else if (value == 0) {
        voice_mic_close(m, sock, VOICE_END_RELEASED);   /* no-op after the cap fired */
    }
    /* value 2 (repeat): ignored */
    return 1;
}

/* Read pending hidraw reports; forward audio frames while the mic is open. */
static void voice_on_hidraw(Mapping *m, int sock) {
    uint8_t report[256];
    while (m->voice_hidfd >= 0) {
        ssize_t n = read(m->voice_hidfd, report, sizeof(report));
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) {
            LOG_INFO("Voice: hidraw for \"%s\" closed (%s)\n", m->name, n == 0 ? "EOF" : strerror(errno));
            voice_close(m, sock, VOICE_END_DISCONNECTED);
            break;
        }
        size_t frame_len = 0;
        const uint8_t *frame = voice_audio_payload(m->voice, report, (size_t)n, &frame_len);
        if (!frame || !m->mic_open) continue;
        uint8_t buf[VOICE_MAX_PKT];
        size_t len = voice_build_audio(buf, sizeof(buf), m->sid, (uint16_t)(m->seq + 1),
                                       (uint32_t)(now_ms() & 0xFFFFFFFF), m->voice->codec, frame, frame_len);
        if (len == 0) {
            LOG_ERROR("Voice: frame of %zu bytes too large for a voice packet, dropped\n", frame_len);
            continue;
        }
        m->seq++;
        m->frames++;
        voice_send(m, sock, buf, len, 1);
    }
}

/* ms until the earliest open mic reaches the cap, or -1 if no mic is open. */
static int voice_next_timeout_ms(void) {
    int best = -1;
    uint64_t now = now_ms();
    for (int i = 0; i < num_mappings; i++) {
        if (!mappings[i].mic_open) continue;
        uint64_t elapsed = now - mappings[i].mic_opened_ms;
        int left = elapsed >= VOICE_MIC_CAP_MS ? 0 : (int)(VOICE_MIC_CAP_MS - elapsed);
        if (best < 0 || left < best) best = left;
    }
    return best;
}

static void voice_enforce_cap(int sock) {
    uint64_t now = now_ms();
    for (int i = 0; i < num_mappings; i++) {
        if (mappings[i].mic_open && now - mappings[i].mic_opened_ms >= VOICE_MIC_CAP_MS) {
            voice_mic_close(&mappings[i], sock, VOICE_END_TIMEOUT);
        }
    }
}

/* -------- Main -------- */
int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        if (load_config(argv[2]) < 0) return 1;
    } else if (argc >= 4) {
        const char *remote_name = argv[1];
        const char *server_ip   = argv[2];
        int server_port         = atoi(argv[3]);
        int edge_repeat_arg = 1;

        if (argc >= 5) {
            if (strcasecmp(argv[4], "TRACE") == 0) log_level = LEVEL_TRACE;
            else if (strcasecmp(argv[4], "DEBUG") == 0) log_level = LEVEL_DEBUG;
            else if (strcasecmp(argv[4], "INFO") == 0)  log_level = LEVEL_INFO;
            else if (strcasecmp(argv[4], "ERROR") == 0) log_level = LEVEL_ERROR;
        }

        if (argc >= 6) {
            edge_repeat_arg = atoi(argv[5]);
            if (edge_repeat_arg < 1) edge_repeat_arg = 1;
        }
        edge_repeat_count = edge_repeat_arg;
        add_mapping(remote_name, server_ip, server_port, NULL);
    } else {
        fprintf(stderr,
            "Remote Bridge version %s\n"
            "Usage: %s <exact_remote_name> <server_ip> <server_port> [LOG_LEVEL] [EDGE_REPEAT_COUNT]\n"
            "   or: %s -c <config_file>\n"
            "\n"
            "Log levels: ERROR, INFO (default), DEBUG, TRACE\n"
            "Edge repeat count: Sends key down/up events X times (default 1)\n",
            VERSION, argv[0], argv[0]);
        return 1;
    }

    LOG_ALWAYS("Remote Bridge version %s\n", VERSION);
    LOG_ALWAYS("Log Level: %s\n", level_to_str(log_level));
    for (int i = 0; i < num_mappings; i++) {
        LOG_ALWAYS("Mapping %d: Remote=\"%s\", Server=%s:%d\n",
            i, mappings[i].name, inet_ntoa(mappings[i].server_addr.sin_addr),
            ntohs(mappings[i].server_addr.sin_port));
    }

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    next_sid = (uint16_t)now_ms();   /* don't reuse recent session ids after a restart */
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* inotify for /dev/input */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        perror("inotify_init");
        return 1;
    }

    if (inotify_add_watch(ifd, INPUT_DIR, IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_TO) < 0) {
        perror("inotify_add_watch");
        // Non-fatal, but we won't see hotplug events
    }
    if (inotify_add_watch(ifd, "/dev", IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_TO) < 0) {
        perror("inotify_add_watch /dev");
    }

    LOG_INFO("Scanning " INPUT_DIR " for remotes...\n");

    /* Initial scan (remotes may already be connected) */
    DIR *d = opendir(INPUT_DIR);
    if (!d) {
        perror("opendir " INPUT_DIR);
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[256];
            snprintf(path, sizeof(path),
                     INPUT_DIR "/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                int m_idx = find_available_mapping(fd, path);
                if (m_idx >= 0) {
                    mappings[m_idx].evfd = fd;
                    strncpy(mappings[m_idx].dev_path, path, sizeof(mappings[m_idx].dev_path) - 1);
                    mappings[m_idx].dev_path[sizeof(mappings[m_idx].dev_path) - 1] = '\0';
                    voice_open_hidraw(&mappings[m_idx]);
                } else {
                    close(fd);
                }
            }
        }
    }
    closedir(d);

    attach_hidraw();

    for (int i = 0; i < num_mappings; i++) {
        if (mappings[i].evfd < 0) {
            LOG_INFO("Mapping %d (\"%s\") not found during initial scan\n", i, mappings[i].name);
        }
    }

    struct pollfd fds[3 * MAX_MAPPINGS + 1];
    int mapping_indices[MAX_MAPPINGS];
    int hid_indices[MAX_MAPPINGS];
    int vhid_indices[MAX_MAPPINGS];
    struct input_event ev;

    while (1) {
        int nfds = 0;
        for (int i = 0; i < num_mappings; i++) {
            if (mappings[i].evfd >= 0) {
                mapping_indices[nfds] = i;
                fds[nfds++] = (struct pollfd){
                    .fd = mappings[i].evfd,
                    .events = POLLIN
                };
            }
        }

        int ifd_idx = nfds;
        fds[nfds++] = (struct pollfd){
            .fd = ifd,
            .events = POLLIN
        };

        int hid_start = nfds;
        for (int i = 0; i < num_mappings; i++) {
            if (mappings[i].hidfd >= 0) {
                hid_indices[nfds - hid_start] = i;
                fds[nfds++] = (struct pollfd){
                    .fd = mappings[i].hidfd,
                    .events = POLLIN
                };
            }
        }

        int vhid_start = nfds;
        int num_vhid = 0;
        for (int i = 0; i < num_mappings; i++) {
            if (mappings[i].voice_hidfd >= 0) {
                vhid_indices[num_vhid++] = i;
                fds[nfds++] = (struct pollfd){
                    .fd = mappings[i].voice_hidfd,
                    .events = POLLIN
                };
            }
        }

        int ret = poll(fds, nfds, voice_next_timeout_ms());
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* ---- evdev events ---- */
        for (int i = 0; i < ifd_idx; i++) {
            int m_idx = mapping_indices[i];
            short revents = fds[i].revents;

            /* Disconnect: kernel raises POLLHUP/POLLERR on BT HID removal */
            if (revents & (POLLHUP | POLLERR)) {
                LOG_INFO("Remote \"%s\" disconnected (POLLHUP/POLLERR)\n", mappings[m_idx].name);
                voice_close(&mappings[m_idx], sock, VOICE_END_DISCONNECTED);
                close(mappings[m_idx].evfd);
                mappings[m_idx].evfd = -1;
                mappings[m_idx].dev_path[0] = '\0';
            } else if (revents & POLLIN) {
                while (1) {
                    ssize_t n = read(mappings[m_idx].evfd, &ev, sizeof(ev));
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;

                    if (n <= 0) {
                        if (n == 0 || errno == ENODEV) {
                            LOG_INFO("Remote \"%s\" disconnected (%s)\n",
                                     mappings[m_idx].name, n == 0 ? "EOF" : "ENODEV");
                            voice_close(&mappings[m_idx], sock, VOICE_END_DISCONNECTED);
                            close(mappings[m_idx].evfd);
                            mappings[m_idx].evfd = -1;
                            mappings[m_idx].dev_path[0] = '\0';
                            break;
                        }
                        perror("read evdev");
                        break;
                    }

                    if (n != sizeof(ev)) {
                        LOG_ERROR("Short read from evdev: expected %zu, got %zd. Closing device.\n", sizeof(ev), n);
                        voice_close(&mappings[m_idx], sock, VOICE_END_DISCONNECTED);
                        close(mappings[m_idx].evfd);
                        mappings[m_idx].evfd = -1;
                        mappings[m_idx].dev_path[0] = '\0';
                        break;
                    }

                    if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
                        mappings[m_idx].pending_scan_code = (uint32_t)ev.value;
                        LOG_TRACE("Scan Event (%s): scan_code=0x%x\n", mappings[m_idx].name, (unsigned)mappings[m_idx].pending_scan_code);
                    } else if (ev.type == EV_KEY) {
                        LOG_TRACE("Key Event (%s): code=%d, value=%d\n", mappings[m_idx].name, ev.code, ev.value);

                        if (mappings[m_idx].voice && ev.code == mappings[m_idx].voice->trigger_key &&
                            voice_on_key(&mappings[m_idx], ev.value, sock)) {
                            if (ev.value == 0) mappings[m_idx].pending_scan_code = 0;
                            continue;
                        }

                        send_key_packet(sock, &mappings[m_idx], ev.code,
                                        mappings[m_idx].pending_scan_code, ev.value);

                        /* Only clear the scan code when key is released (value == 0) */
                        if (ev.value == 0) {
                            mappings[m_idx].pending_scan_code = 0;
                        }
                    }
                }
            }
        }

        /* ---- hidraw app-button reports ---- */
        for (int i = hid_start; i < vhid_start; i++) {
            Mapping *m = &mappings[hid_indices[i - hid_start]];
            if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                LOG_INFO("hidraw %s for \"%s\" disconnected (POLLHUP/POLLERR)\n", m->hid_path, m->name);
                close_hidraw(m);
            } else if (fds[i].revents & POLLIN) {
                handle_hidraw(sock, m);
            }
        }

        /* ---- hidraw (voice) reports ---- */
        for (int i = 0; i < num_vhid; i++) {
            Mapping *m = &mappings[vhid_indices[i]];
            short revents = fds[vhid_start + i].revents;
            if (m->voice_hidfd != fds[vhid_start + i].fd) continue;   /* closed meanwhile */
            if (revents & (POLLHUP | POLLERR)) {
                LOG_INFO("Voice: hidraw for \"%s\" hung up\n", m->name);
                voice_close(m, sock, VOICE_END_DISCONNECTED);
            } else if (revents & POLLIN) {
                voice_on_hidraw(m, sock);
            }
        }
        voice_enforce_cap(sock);

        /* ---- inotify events ---- */
        if (fds[ifd_idx].revents & POLLIN) {
            char buf[4096];
            ssize_t len = read(ifd, buf, sizeof(buf));
            if (len < 0) {
                if (errno != EAGAIN) perror("read inotify");
            } else {
                for (char *p = buf; p < buf + len; ) {
                    struct inotify_event *ie = (struct inotify_event *)p;

                    if (ie->len && strncmp(ie->name, "hidraw", 6) == 0) {
                        char path[256];
                        snprintf(path, sizeof(path), "/dev/%s", ie->name);

                        if (ie->mask & IN_DELETE) {
                            for (int i = 0; i < num_mappings; i++) {
                                if (mappings[i].hidfd >= 0 && strcmp(path, mappings[i].hid_path) == 0) {
                                    LOG_INFO("hidraw %s for \"%s\" removed (inotify)\n", path, mappings[i].name);
                                    close_hidraw(&mappings[i]);
                                }
                            }
                        }
                        if (ie->mask & (IN_CREATE | IN_ATTRIB | IN_MOVED_TO)) attach_hidraw();
                    }

                    if (ie->len && strncmp(ie->name, "event", 5) == 0) {
                        char path[256];
                        snprintf(path, sizeof(path), INPUT_DIR "/%s", ie->name);

                        if ((ie->mask & IN_DELETE)) {
                            for (int i = 0; i < num_mappings; i++) {
                                if (mappings[i].evfd >= 0 && strcmp(path, mappings[i].dev_path) == 0) {
                                    LOG_INFO("Remote \"%s\" disconnected (inotify: %s)\n", mappings[i].name, path);
                                    voice_close(&mappings[i], sock, VOICE_END_DISCONNECTED);
                                    close(mappings[i].evfd);
                                    mappings[i].evfd = -1;
                                    mappings[i].dev_path[0] = '\0';
                                }
                            }
                        }

                        if ((ie->mask & (IN_CREATE | IN_ATTRIB | IN_MOVED_TO))) {
                            int fd = open(path, O_RDONLY | O_NONBLOCK);
                            if (fd >= 0) {
                                int m_idx = find_available_mapping(fd, path);
                                if (m_idx >= 0) {
                                    mappings[m_idx].evfd = fd;
                                    strncpy(mappings[m_idx].dev_path, path, sizeof(mappings[m_idx].dev_path) - 1);
                                    mappings[m_idx].dev_path[sizeof(mappings[m_idx].dev_path) - 1] = '\0';
                                    voice_open_hidraw(&mappings[m_idx]);
                                } else {
                                    close(fd);
                                }
                            }
                        }
                    }

                    p += sizeof(*ie) + ie->len;
                }
                attach_hidraw();
            }
        }
    }

    return 0;
}
#endif /* REMOTE_BRIDGE_TEST */
