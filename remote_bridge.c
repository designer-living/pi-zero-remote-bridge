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
#include <linux/input.h>
#include <sys/inotify.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define INPUT_DIR "/dev/input"
#define MAX_NAME 256

/* -------- Binary packet -------- */
struct Packet {
    uint32_t timestamp_ms;  // network byte order
    uint16_t key_code;      // network byte order
    int8_t   value;         // 0=up, 1=down, 2=repeat
    uint8_t  reserved;
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

/* -------- Logging helper with UTC timestamp -------- */
static void log_print(int level, const char *level_name, const char *fmt, ...) {
    if (level != LEVEL_ALWAYS && level > log_level) return;

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char tstr[32];
    strftime(tstr, sizeof(tstr), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

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

/* -------- Match exact evdev name -------- */
static int matches_device(int fd, const char *target, const char *path) {
    char name[MAX_NAME] = {0};

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        LOG_DEBUG("Failed to get name for %s: %s\n", path, strerror(errno));
        return 0;
    }

    LOG_DEBUG("Checking device %s: name=\"%s\"\n", path, name);

    if (strcmp(name, target) == 0) {
        LOG_INFO("Matched device: %s at %s\n", name, path);
        return 1;
    }

    return 0;
}

static int try_open_device(const char *path, const char *target) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != EACCES) {
            LOG_DEBUG("Failed to open %s: %s\n", path, strerror(errno));
        } else {
            LOG_DEBUG("Permission denied for %s (try sudo?)\n", path);
        }
        return -1;
    }

    if (matches_device(fd, target, path))
        return fd;

    close(fd);
    return -1;
}

/* -------- Main -------- */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Remote Bridge version %s\n"
            "Usage: %s <exact_remote_name> <server_ip> <server_port> [LOG_LEVEL]\n"
            "\n"
            "Log levels: ERROR, INFO (default), DEBUG, TRACE\n",
            VERSION, argv[0]);
        return 1;
    }

    const char *remote_name = argv[1];
    const char *server_ip   = argv[2];
    int server_port         = atoi(argv[3]);

    if (argc >= 5) {
        if (strcasecmp(argv[4], "TRACE") == 0) log_level = LEVEL_TRACE;
        else if (strcasecmp(argv[4], "DEBUG") == 0) log_level = LEVEL_DEBUG;
        else if (strcasecmp(argv[4], "INFO") == 0)  log_level = LEVEL_INFO;
        else if (strcasecmp(argv[4], "ERROR") == 0) log_level = LEVEL_ERROR;
    }

    LOG_ALWAYS("Remote Bridge version %s\n", VERSION);
    LOG_ALWAYS("Remote Name: \"%s\"\n", remote_name);
    LOG_ALWAYS("Server IP:   \"%s\"\n", server_ip);
    LOG_ALWAYS("Server Port: %d\n", server_port);
    LOG_ALWAYS("Log Level:   %s\n", level_to_str(log_level));

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port   = htons(server_port);
    if (!inet_aton(server_ip, &server.sin_addr)) {
        LOG_ERROR("Invalid IP address: %s\n", server_ip);
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

    int evfd = -1;

    LOG_INFO("Waiting for remote matching \"%s\"...\n", remote_name);

    /* Initial scan (remote may already be connected) */
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
            evfd = try_open_device(path, remote_name);
            if (evfd >= 0) {
                LOG_INFO("Remote connected\n");
                LOG_DEBUG("Device node opened at %s during initial scan\n", path);
                break;
            }
        }
    }
    closedir(d);

    if (evfd < 0) {
        LOG_DEBUG("Remote not found during initial scan of " INPUT_DIR "\n");
    }

    struct pollfd fds[2];
    struct input_event ev;
    struct Packet pkt;

    while (1) {
        int nfds = 0;
        int evfd_idx = -1;
        int ifd_idx  = -1;

        if (evfd >= 0) {
            evfd_idx = nfds;
            fds[nfds++] = (struct pollfd){
                .fd = evfd,
                .events = POLLIN
            };
        }

        ifd_idx = nfds;
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
        if (evfd_idx >= 0) {
            short revents = fds[evfd_idx].revents;

            /* Disconnect: kernel raises POLLHUP/POLLERR on BT HID removal */
            if (revents & (POLLHUP | POLLERR)) {
                LOG_INFO("Remote disconnected (POLLHUP/POLLERR on evdev fd)\n");
                LOG_DEBUG("Closing stale evdev fd %d\n", evfd);
                close(evfd);
                evfd = -1;
            } else if (revents & POLLIN) {
                while (1) {
                    ssize_t n = read(evfd, &ev, sizeof(ev));
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EINTR) break;
                        if (errno == ENODEV) {
                            LOG_INFO("Remote disconnected (ENODEV on read)\n");
                            LOG_DEBUG("Closing evdev fd %d\n", evfd);
                            close(evfd);
                            evfd = -1;
                            break;
                        }
                        perror("read evdev");
                        break;
                    }

                    if (n != sizeof(ev)) {
                        LOG_DEBUG("Short read from evdev: expected %zu, got %zd\n", sizeof(ev), n);
                        break;
                    }

                    if (ev.type == EV_KEY) {
                        LOG_TRACE("Key Event: code=%d, value=%d\n", ev.code, ev.value);
                        pkt.timestamp_ms =
                            htonl((uint32_t)(now_ms() & 0xFFFFFFFF));
                        pkt.key_code = htons((uint16_t)ev.code);
                        pkt.value    = (int8_t)ev.value;
                        pkt.reserved = 0;

                        ssize_t sent = sendto(sock,
                                              &pkt, sizeof(pkt), 0,
                                              (struct sockaddr *)&server,
                                              sizeof(server));
                        if (sent < 0) {
                            LOG_ERROR("Failed to send UDP packet: %s\n", strerror(errno));
                        } else {
                            LOG_TRACE("Sent UDP packet for key %d, value %d\n", ev.code, ev.value);
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

                        if ((ie->mask & IN_DELETE) && evfd >= 0) {
                            char deleted_path[256];
                            snprintf(deleted_path, sizeof(deleted_path), INPUT_DIR "/%s", ie->name);

                            char evfd_path[256];
                            char evfd_proc_path[64];
                            snprintf(evfd_proc_path, sizeof(evfd_proc_path), "/proc/self/fd/%d", evfd);
                            ssize_t path_len = readlink(evfd_proc_path, evfd_path, sizeof(evfd_path) - 1);

                            if (path_len != -1) {
                                evfd_path[path_len] = '\0';
                                if (strcmp(deleted_path, evfd_path) == 0) {
                                    /* Belt-and-braces: catch delete if POLLHUP missed it */
                                    LOG_DEBUG("inotify IN_DELETE for our device %s, closing evdev fd\n", ie->name);
                                    close(evfd);
                                    evfd = -1;
                                }
                            }
                        }

                        if ((ie->mask & (IN_CREATE | IN_ATTRIB | IN_MOVED_TO)) &&
                            evfd < 0) {
                            char path[256];
                            snprintf(path, sizeof(path),
                                     INPUT_DIR "/%s", ie->name);

                            LOG_DEBUG("Hotplug event (mask=0x%x): %s\n", ie->mask, path);

                            evfd = try_open_device(path, remote_name);
                            if (evfd >= 0) {
                                LOG_INFO("Remote reconnected\n");
                                LOG_DEBUG("Device node opened at %s\n", path);
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
