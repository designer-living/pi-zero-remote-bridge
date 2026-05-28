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
#endif
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define INPUT_DIR "/dev/input"
#define MAX_NAME 256

#define PACKET_FORMAT_VERSION    1
#define PKT_MAKE_HEADER(ver)     ((uint8_t)(((ver) & 0x0F) << 4))
#define PKT_HEADER_VERSION(h)    (((h) >> 4) & 0x0F)

/* -------- Binary packet -------- */
struct Packet {
    uint8_t  header;        /* upper 4 bits = format version (0-15), lower 4 bits reserved */
    uint32_t timestamp_ms;  /* network byte order */
    uint16_t key_code;      /* network byte order */
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

typedef struct {
    char name[MAX_NAME];
    char dev_path[256];
    struct sockaddr_in server_addr;
    int evfd;
    int repeat_delay_ms;
    uint64_t last_repeat_send_ms;
} Mapping;

static Mapping mappings[MAX_MAPPINGS];
static int num_mappings = 0;

static int add_mapping(const char *name, const char *ip, int port, int delay) {
    if (num_mappings >= MAX_MAPPINGS) {
        LOG_ERROR("Maximum number of mappings (%d) reached\n", MAX_MAPPINGS);
        return -1;
    }
    Mapping *m = &mappings[num_mappings++];
    strncpy(m->name, name, MAX_NAME - 1);
    m->name[MAX_NAME - 1] = '\0';
    m->dev_path[0] = '\0';
    m->evfd = -1;
    m->repeat_delay_ms = delay;
    m->last_repeat_send_ms = 0;

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

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        LOG_DEBUG("Failed to get name for %s: %s\n", path, strerror(errno));
        return -1;
    }

    LOG_DEBUG("Checking device %s: name=\"%s\"\n", path, name);

    for (int i = 0; i < num_mappings; i++) {
        if (mappings[i].evfd < 0) {
            if (strcmp(mappings[i].name, name) == 0) {
                LOG_INFO("Matched device: %s at %s for mapping %d\n", name, path, i);
                return i;
            } else {
                LOG_TRACE("Mapping %d name (\"%s\") does not match device name (\"%s\")\n", 
                    i, mappings[i].name, name);
            }
        }
    }

    return -1;
}

static int load_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        LOG_ERROR("Failed to open config file %s: %s\n", filename, strerror(errno));
        return -1;
    }

    char line[512];
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
        } else if (strcasecmp(key, "REMOTE") == 0) {
            char name[MAX_NAME], ip[64];
            int port, delay = 0;
            // Skip leading/trailing spaces in the sscanf fields
            int n = sscanf(val, " %255[^,] , %63[^,] , %d , %d", name, ip, &port, &delay);
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

                if (add_mapping(name, ip, port, delay) == 0) {
                    LOG_DEBUG("Loaded mapping: Remote=\"%s\", Server=%s:%d, Delay=%dms\n",
                        name, ip, port, delay);
                }
            } else {
                LOG_ERROR("Invalid REMOTE line: %s=%s\n", key, val);
            }
        }
    }
    fclose(f);
    return 0;
}

/* -------- Main -------- */
int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        if (load_config(argv[2]) < 0) return 1;
    } else if (argc >= 4) {
        const char *remote_name = argv[1];
        const char *server_ip   = argv[2];
        int server_port         = atoi(argv[3]);
        int delay = 0;

        if (argc >= 5) {
            if (strcasecmp(argv[4], "TRACE") == 0) log_level = LEVEL_TRACE;
            else if (strcasecmp(argv[4], "DEBUG") == 0) log_level = LEVEL_DEBUG;
            else if (strcasecmp(argv[4], "INFO") == 0)  log_level = LEVEL_INFO;
            else if (strcasecmp(argv[4], "ERROR") == 0) log_level = LEVEL_ERROR;
        }

        if (argc >= 6) {
            delay = atoi(argv[5]);
        }
        add_mapping(remote_name, server_ip, server_port, delay);
    } else {
        fprintf(stderr,
            "Remote Bridge version %s\n"
            "Usage: %s <exact_remote_name> <server_ip> <server_port> [LOG_LEVEL] [REPEAT_DELAY_MS]\n"
            "   or: %s -c <config_file>\n"
            "\n"
            "Log levels: ERROR, INFO (default), DEBUG, TRACE\n"
            "Repeat delay: Throttles repeat events (value=2) to every X ms (default 0=disabled)\n",
            VERSION, argv[0], argv[0]);
        return 1;
    }

    LOG_ALWAYS("Remote Bridge version %s\n", VERSION);
    LOG_ALWAYS("Log Level: %s\n", level_to_str(log_level));
    for (int i = 0; i < num_mappings; i++) {
        LOG_ALWAYS("Mapping %d: Remote=\"%s\", Server=%s:%d, Delay=%dms\n",
            i, mappings[i].name, inet_ntoa(mappings[i].server_addr.sin_addr),
            ntohs(mappings[i].server_addr.sin_port), mappings[i].repeat_delay_ms);
    }

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
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
                } else {
                    close(fd);
                }
            }
        }
    }
    closedir(d);

    for (int i = 0; i < num_mappings; i++) {
        if (mappings[i].evfd < 0) {
            LOG_INFO("Mapping %d (\"%s\") not found during initial scan\n", i, mappings[i].name);
        }
    }

    struct pollfd fds[MAX_MAPPINGS + 1];
    int mapping_indices[MAX_MAPPINGS];
    struct input_event ev;
    struct Packet pkt;

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

        int ret = poll(fds, nfds, -1);
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
                        close(mappings[m_idx].evfd);
                        mappings[m_idx].evfd = -1;
                        mappings[m_idx].dev_path[0] = '\0';
                        break;
                    }

                    if (ev.type == EV_KEY) {
                        LOG_TRACE("Key Event (%s): code=%d, value=%d\n", mappings[m_idx].name, ev.code, ev.value);

                        /* Throttle repeat events (value=2) if repeat_delay_ms is set */
                        if (ev.value == 2 && mappings[m_idx].repeat_delay_ms > 0) {
                            uint64_t now = now_ms();
                            if (now - mappings[m_idx].last_repeat_send_ms < (uint64_t)mappings[m_idx].repeat_delay_ms) {
                                LOG_TRACE("Throttling repeat event for key %d\n", ev.code);
                                continue;
                            }
                            mappings[m_idx].last_repeat_send_ms = now;
                        }

                        pkt.header       = PKT_MAKE_HEADER(PACKET_FORMAT_VERSION);
                        pkt.timestamp_ms = htonl((uint32_t)(now_ms() & 0xFFFFFFFF));
                        pkt.key_code     = htons((uint16_t)ev.code);
                        pkt.value        = (int8_t)ev.value;

                        ssize_t sent = sendto(sock,
                                              &pkt, sizeof(pkt), 0,
                                              (struct sockaddr *)&mappings[m_idx].server_addr,
                                              sizeof(struct sockaddr_in));
                        if (sent < 0) {
                            LOG_ERROR("Failed to send UDP packet: %s\n", strerror(errno));
                        } else {
                            LOG_TRACE("Sent UDP packet for key %d, value %d to %s:%d\n",
                                ev.code, ev.value,
                                inet_ntoa(mappings[m_idx].server_addr.sin_addr),
                                ntohs(mappings[m_idx].server_addr.sin_port));
                        }
                    }
                }
            }
        }

        /* ---- inotify events ---- */
        if (fds[ifd_idx].revents & POLLIN) {
            char buf[4096];
            ssize_t len = read(ifd, buf, sizeof(buf));
            if (len < 0) {
                if (errno != EAGAIN) perror("read inotify");
            } else {
                for (char *p = buf; p < buf + len; ) {
                    struct inotify_event *ie = (struct inotify_event *)p;

                    if (ie->len && strncmp(ie->name, "event", 5) == 0) {
                        char path[256];
                        snprintf(path, sizeof(path), INPUT_DIR "/%s", ie->name);

                        if ((ie->mask & IN_DELETE)) {
                            for (int i = 0; i < num_mappings; i++) {
                                if (mappings[i].evfd >= 0 && strcmp(path, mappings[i].dev_path) == 0) {
                                    LOG_INFO("Remote \"%s\" disconnected (inotify: %s)\n", mappings[i].name, path);
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
                                } else {
                                    close(fd);
                                }
                            }
                        }
                    }

                    p += sizeof(*ie) + ie->len;
                }
            }
        }
    }

    return 0;
}
