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

/* CI and the build container have no system bus: the no-bus path. */
static void test_bluez_without_bus(void) {
    CHECK(bluez_open() == -1);
    CHECK(bluez_fd() == -1);
    CHECK(bluez_poll_events() == 0);
    bluez_process();
    CHECK(bluez_read_battery("dev_18_BF_1C_D3_0B_A0") == -1);
    CHECK(bluez_read_battery_at("/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0") == -1);
}

/* Read one status message; returns its length (or -1) and fills the fields. */
static ssize_t read_status(int cap, uint8_t *kind, uint8_t *battery, uint32_t *session_ms) {
    uint8_t buf[64];
    ssize_t n = capture_read(cap, buf, sizeof buf);
    if (n != PKT_STATUS_LEN || buf[0] != 0x30 || buf[5] != PKT_MSG_STATUS) return n < 0 ? -1 : 0;
    *kind = buf[6];
    *battery = buf[7];
    *session_ms = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 8) | buf[11];
    return n;
}

/* Attach m to a dummy fd as if its evdev device had just appeared. */
static void attach(Mapping *m, const char *uniq) {
    int fd = open("/dev/null", O_RDONLY);
    remote_attached(g_sock, (int)(m - mappings), fd, "/dev/input/event9", uniq);
}

static void test_usb_attach_detach(void) {
    uint8_t kind, bat; uint32_t sess;
    Mapping *m = test_mapping("USB Remote");
    int cap = capture_open(m);

    attach(m, "");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_CONNECTED && bat == BATTERY_UNKNOWN && sess == 0);
    CHECK(m->bt_suffix[0] == '\0' && m->evfd >= 0);

    remote_detached(g_sock, m, "test");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_DISCONNECTED && bat == BATTERY_UNKNOWN && sess < 1000);
    CHECK(m->evfd == -1 && m->dev_path[0] == '\0');
    CHECK(read_status(cap, &kind, &bat, &sess) == -1);
    close(cap);
}

static void test_battery_dedupe_and_reset(void) {
    uint8_t kind, bat; uint32_t sess;
    Mapping *m = test_mapping("BT Remote");
    int cap = capture_open(m);

    attach(m, "18:bf:1c:d3:0b:a0");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_CONNECTED && bat == BATTERY_UNKNOWN);   /* no bus */
    CHECK(strcmp(m->bt_suffix, "dev_18_BF_1C_D3_0B_A0") == 0);

    remote_battery(g_sock, m, 39);
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN);
    CHECK(kind == STATUS_BATTERY && bat == 39 && sess == 0);
    remote_battery(g_sock, m, 39);
    CHECK(read_status(cap, &kind, &bat, &sess) == -1);
    remote_battery(g_sock, m, 38);
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN && kind == STATUS_BATTERY && bat == 38);
    remote_battery(g_sock, m, 101);
    remote_battery(g_sock, m, -1);
    CHECK(read_status(cap, &kind, &bat, &sess) == -1);

    remote_detached(g_sock, m, "test");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN && kind == STATUS_DISCONNECTED && bat == 38);
    remote_battery(g_sock, m, 50);                                /* not attached */
    CHECK(read_status(cap, &kind, &bat, &sess) == -1);

    attach(m, "18:bf:1c:d3:0b:a0");
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN && kind == STATUS_CONNECTED);
    remote_battery(g_sock, m, 38);                                /* dedupe resets per connection */
    CHECK(read_status(cap, &kind, &bat, &sess) == PKT_STATUS_LEN && kind == STATUS_BATTERY && bat == 38);
    remote_detached(g_sock, m, "test");
    read_status(cap, &kind, &bat, &sess);
    close(cap);
}

static void test_battery_signal_routes_by_address(void) {
    uint8_t kind, bat; uint32_t sess;
    Mapping *a = test_mapping("Remote A");
    Mapping *b = test_mapping("Remote B");
    int cap_a = capture_open(a), cap_b = capture_open(b);
    attach(a, "18:bf:1c:d3:0b:a0");
    attach(b, "11:22:33:44:55:66");
    read_status(cap_a, &kind, &bat, &sess);
    read_status(cap_b, &kind, &bat, &sess);

    route_battery(g_sock, "/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0", 55);
    CHECK(read_status(cap_a, &kind, &bat, &sess) == PKT_STATUS_LEN && kind == STATUS_BATTERY && bat == 55);
    CHECK(read_status(cap_b, &kind, &bat, &sess) == -1);

    route_battery(g_sock, "/org/bluez/hci0/dev_18_BF_1C_D3_0B_A0/service0012", 60);
    route_battery(g_sock, "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", 60);
    CHECK(read_status(cap_a, &kind, &bat, &sess) == -1);
    CHECK(read_status(cap_b, &kind, &bat, &sess) == -1);

    remote_detached(g_sock, a, "test");
    remote_detached(g_sock, b, "test");
    close(cap_a); close(cap_b);
}

int main(void) {
    log_level = LEVEL_ERROR;
    test_key_is_v3();
    test_bluez_without_bus();
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    test_usb_attach_detach();
    test_battery_dedupe_and_reset();
    test_battery_signal_routes_by_address();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all bridge tests passed\n");
    return 0;
}
