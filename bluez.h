#ifndef BLUEZ_H
#define BLUEZ_H

/* Battery levels of Bluetooth remotes, read from bluetoothd over the system
 * D-Bus (basu / sd-bus). Only reads org.bluez.Battery1.Percentage, which
 * bluetoothd already holds, and listens for its change signals: nothing
 * here sends anything to a remote, so it costs the remote no battery. */

/* Open the system bus and subscribe to BlueZ signals. No-op (0) if already
 * open; -1 if the bus can't be reached (logged once per failure streak). */
int bluez_open(void);

/* Bus fd and the poll events it wants; -1 / 0 while closed. */
int bluez_fd(void);
short bluez_poll_events(void);

/* Handle everything the bus has queued (signals call the battery handler).
 * On a bus error, logs it and closes the bus. Safe while closed. */
void bluez_process(void);

/* Battery percentage of the device at a BlueZ object path, or of
 * /org/bluez/hci0..3/<dev_suffix>. 0-100, or -1 if unknown or closed. */
int bluez_read_battery_at(const char *object_path);
int bluez_read_battery(const char *dev_suffix);

/* Called with a device's object path when its battery may have changed
 * (Battery1 properties changed, or a Battery1 interface appeared). */
void bluez_set_battery_handler(void (*fn)(const char *object_path));

#endif
