#include "bluez.h"
#include "log.h"

#include <basu/sd-bus.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BATTERY_IFACE "org.bluez.Battery1"
#define BUS_CALL_TIMEOUT_US (500 * 1000)

static sd_bus *bus;
static int open_failed;                         /* last bluez_open failed: don't log again */
static void (*battery_handler)(const char *object_path);

void bluez_set_battery_handler(void (*fn)(const char *object_path)) {
    battery_handler = fn;
}

static void notify(const char *object_path) {
    if (battery_handler && object_path) battery_handler(object_path);
}

/* PropertiesChanged(s interface, a{sv} changed, as invalidated) on a device path */
static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *iface = NULL;
    if (sd_bus_message_read(m, "s", &iface) >= 0 && iface && strcmp(iface, BATTERY_IFACE) == 0) {
        notify(sd_bus_message_get_path(m));
    }
    return 0;
}

/* InterfacesAdded(o path, a{sa{sv}} interfaces): only care if Battery1 is among them */
static int on_interfaces_added(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *path = NULL;
    if (sd_bus_message_read(m, "o", &path) < 0 || !path) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
        const char *iface = NULL;
        if (sd_bus_message_read(m, "s", &iface) < 0) break;
        if (iface && strcmp(iface, BATTERY_IFACE) == 0) {
            notify(path);
            return 0;
        }
        sd_bus_message_skip(m, "a{sv}");
        sd_bus_message_exit_container(m);
    }
    return 0;
}

static void close_bus(void) {
    if (bus) {
        /* No flush: it would first wait for a half-open connection to finish. */
        sd_bus_close(bus);
        sd_bus_unref(bus);
        bus = NULL;
    }
}

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* Finish connecting (authentication + Hello) within BUS_CALL_TIMEOUT_US.
 * sd-bus would otherwise wait up to 90 s for that inside the first call. */
static int wait_until_ready(void) {
    uint64_t deadline = mono_us() + BUS_CALL_TIMEOUT_US;
    for (;;) {
        int r = sd_bus_is_ready(bus);
        if (r != 0) return r < 0 ? r : 0;
        uint64_t now = mono_us();
        if (now >= deadline) return -ETIMEDOUT;
        r = sd_bus_process(bus, NULL);
        if (r < 0) return r;
        if (r > 0) continue;
        r = sd_bus_wait(bus, deadline - now);
        if (r < 0) return r;
    }
}

int bluez_open(void) {
    if (bus) return 0;
    /* Every call here runs on the single main loop: the bus and bluetoothd
     * answer in milliseconds, so give up quickly rather than stall keys. */
    int r = sd_bus_open_system(&bus);
    if (r >= 0) r = wait_until_ready();
    if (r >= 0) r = sd_bus_set_method_call_timeout(bus, BUS_CALL_TIMEOUT_US);
    if (r >= 0) {
        r = sd_bus_match_signal(bus, NULL, "org.bluez", NULL,
                                "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                on_properties_changed, NULL);
    }
    if (r >= 0) {
        r = sd_bus_match_signal(bus, NULL, "org.bluez", NULL,
                                "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                on_interfaces_added, NULL);
    }
    if (r < 0) {
        if (!open_failed) {
            LOG_ERROR("Battery: can't use the system D-Bus (%s); remotes will report battery as unknown\n",
                      strerror(-r));
        }
        open_failed = 1;
        close_bus();
        return -1;
    }
    open_failed = 0;
    LOG_INFO("Battery: listening to BlueZ on the system D-Bus\n");
    return 0;
}

int bluez_fd(void) {
    return bus ? sd_bus_get_fd(bus) : -1;
}

short bluez_poll_events(void) {
    if (!bus) return 0;
    int ev = sd_bus_get_events(bus);
    return ev < 0 ? POLLIN : (short)(ev | POLLIN);
}

void bluez_process(void) {
    while (bus) {
        int r = sd_bus_process(bus, NULL);
        if (r < 0) {
            LOG_ERROR("Battery: lost the system D-Bus (%s); will retry when a remote connects\n",
                      strerror(-r));
            close_bus();
            return;
        }
        if (r == 0) return;
    }
}

int bluez_read_battery_at(const char *object_path) {
    if (!bus || !object_path) return -1;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    uint8_t pct = 0;
    int r = sd_bus_get_property_trivial(bus, "org.bluez", object_path, BATTERY_IFACE,
                                        "Percentage", &err, 'y', &pct);
    if (r < 0) {
        LOG_DEBUG("Battery: no level at %s (%s)\n", object_path,
                  err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return -1;
    }
    sd_bus_error_free(&err);
    return pct <= 100 ? pct : -1;
}

int bluez_read_battery(const char *dev_suffix) {
    if (!bus || !dev_suffix || !*dev_suffix) return -1;
    char path[96];
    for (int hci = 0; hci < 4; hci++) {
        snprintf(path, sizeof(path), "/org/bluez/hci%d/%s", hci, dev_suffix);
        int pct = bluez_read_battery_at(path);
        if (pct >= 0) return pct;
    }
    return -1;
}
