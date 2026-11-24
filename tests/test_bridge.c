/* Unit tests for what remote_bridge.c sends to the gateway. Packets go to a
 * UDP socket on 127.0.0.1 and are read back. Run with `make test`. */
#define REMOTE_BRIDGE_TEST
#include "../remote_bridge.c"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* A mapping whose server address will be pointed at a capture socket. */
static Mapping *test_mapping(const char *name) {
    if (add_mapping(name, "127.0.0.1", 1, NULL) < 0) return NULL;
    return &mappings[num_mappings - 1];
}

/* Bind a non-blocking UDP socket on 127.0.0.1 and point m's server at it. */
static int capture_open(Mapping *m) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    m->server_addr = a;
    return fd;
}

/* One datagram, or -1 if none is waiting. */
static ssize_t capture_read(int fd, uint8_t *buf, size_t cap) {
    usleep(2000);
    ssize_t n = recv(fd, buf, cap, 0);
    return n < 0 ? -1 : n;
}

static void test_key_is_v3(void) {
    Mapping *m = test_mapping("Key Remote");
    int cap = capture_open(m);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t buf[64];

    edge_repeat_count = 2;
    send_key_packet(sock, m, 0x0067, 0x000C0033, 1);
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);
    CHECK(buf[0] == 0x30 && buf[5] == PKT_MSG_KEY);
    CHECK(buf[6] == 0x00 && buf[7] == 0x67);
    CHECK(buf[8] == 0x00 && buf[9] == 0x0C && buf[10] == 0x00 && buf[11] == 0x33);
    CHECK(buf[12] == 1);
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);   /* edge repeat */
    send_key_packet(sock, m, 0x0067, 0, 2);                     /* hold: sent once */
    CHECK(capture_read(cap, buf, sizeof buf) == PKT_KEY_LEN);
    CHECK(capture_read(cap, buf, sizeof buf) == -1);
    edge_repeat_count = 1;
    close(sock);
    close(cap);
}

int main(void) {
    log_level = LEVEL_ERROR;
    test_key_is_v3();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all bridge tests passed\n");
    return 0;
}
