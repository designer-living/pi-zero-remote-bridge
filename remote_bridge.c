#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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
#include <sys/poll.h>
#include <time.h>

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

static int debug = 0;

#define DEBUG_PRINT(fmt, ...) \
    do { if (debug) { printf("[DEBUG] " fmt, ##__VA_ARGS__); fflush(stdout); } } while (0)

/* -------- Match exact evdev name -------- */
static int matches_device(int fd, const char *target, const char *path) {
    char name[MAX_NAME] = {0};

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        DEBUG_PRINT("Failed to get name for %s: %s\n", path, strerror(errno));
        return 0;
    }

    DEBUG_PRINT("Checking device %s: name=\"%s\"\n", path, name);

    if (strcmp(name, target) == 0) {
        printf("Matched device: %s at %s\n", name, path);
        fflush(stdout);
        return 1;
    }

    return 0;
}

static int try_open_device(const char *path, const char *target) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != EACCES) {
            DEBUG_PRINT("Failed to open %s: %s\n", path, strerror(errno));
        } else {
            DEBUG_PRINT("Permission denied for %s (try sudo?)\n", path);
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
            "Usage: %s <exact_remote_name> <server_ip> <server_port> [--debug]\n",
            argv[0]);
        return 1;
    }

    const char *remote_name = argv[1];
    const char *server_ip   = argv[2];
    int server_port         = atoi(argv[3]);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
            break;
        }
    }

    if (debug) printf("Debug logging enabled\n");
    printf("Connecting to %s:%d\n", server_ip, server_port);
    fflush(stdout);

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
        fprintf(stderr, "Invalid IP address: %s\n", server_ip);
        return 1;
    }

    /* inotify for /dev/input */
    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) {
        perror("inotify_init");
        return 1;
    }

    if (inotify_add_watch(ifd, INPUT_DIR, IN_CREATE | IN_DELETE) < 0) {
        perror("inotify_add_watch");
        // Non-fatal, but we won't see hotplug events
    }

    int evfd = -1;

    printf("Waiting for remote matching \"%s\"...\n", remote_name);
    fflush(stdout);

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
            if (evfd >= 0)
                break;
        }
    }
    closedir(d);

    if (evfd < 0) {
        DEBUG_PRINT("Remote not found during initial scan of " INPUT_DIR "\n");
    }

    struct pollfd fds[2];
    struct input_event ev;
    struct Packet pkt;

    while (1) {
        int nfds = 0;

        if (evfd >= 0) {
            fds[nfds++] = (struct pollfd){
                .fd = evfd,
                .events = POLLIN
            };
        }

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

        int idx = 0;

        /* ---- evdev events ---- */
        if (evfd >= 0 && fds[idx++].revents & POLLIN) {
            while (1) {
                ssize_t n = read(evfd, &ev, sizeof(ev));
                if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR)
                        break;
                    if (errno == ENODEV) {
                        printf("Remote disconnected\n");
                        fflush(stdout);
                        close(evfd);
                        evfd = -1;
                        break;
                    }
                    perror("read evdev");
                    break;
                }

                if (n != sizeof(ev)) {
                    DEBUG_PRINT("Short read from evdev: expected %zu, got %zd\n", sizeof(ev), n);
                    break;
                }

                DEBUG_PRINT("Event: type=%d, code=%d, value=%d\n", ev.type, ev.code, ev.value);

                if (ev.type == EV_KEY) {
                    pkt.timestamp_ms =
                        htonl((uint32_t)(now_ms() & 0xFFFFFFFF));
                    pkt.key_code = htons((uint16_t)ev.code);
                    pkt.value    = (int8_t)ev.value;
                    pkt.reserved = 0;

                    ssize_t sent = sendto(sock,
                                          &pkt,
                                          sizeof(pkt),
                                          0,
                                          (struct sockaddr *)&server,
                                          sizeof(server));
                    if (sent < 0) {
                        DEBUG_PRINT("Failed to send UDP packet: %s\n", strerror(errno));
                    } else {
                        DEBUG_PRINT("Sent UDP packet for key %d, value %d\n", ev.code, ev.value);
                    }
                }
            }
        }

        /* ---- inotify events ---- */
        if (fds[idx].revents & POLLIN) {
            char buf[4096];
            ssize_t len = read(ifd, buf, sizeof(buf));
            if (len < 0) {
                if (errno != EAGAIN) perror("read inotify");
            } else {
                for (char *p = buf; p < buf + len; ) {
                    struct inotify_event *ie =
                        (struct inotify_event *)p;

                    if (ie->len &&
                        strncmp(ie->name, "event", 5) == 0 &&
                        (ie->mask & IN_CREATE) &&
                        evfd < 0) {

                        char path[256];
                        snprintf(path, sizeof(path),
                                 INPUT_DIR "/%s", ie->name);

                        DEBUG_PRINT("Hotplug detected: %s\n", path);
                        evfd = try_open_device(path, remote_name);
                        if (evfd >= 0) {
                            printf("Remote connected\n");
                            fflush(stdout);
                        }
                    }

                    p += sizeof(*ie) + ie->len;
                }
            }
        }
    }

    return 0;
}
