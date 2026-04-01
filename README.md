# Remote Bridge - Setup Guide

## Installing from Packages

Pre-built packages for Debian/Raspberry Pi OS and Alpine Linux are available on the
[Releases](../../releases) page for `armhf`, `arm64`, and `amd64`.

### Debian / Raspberry Pi OS

```bash
# Download the package for your architecture (e.g. armhf for Pi Zero v1/v2)
wget https://github.com/foxy82/pi-zero-remote-bridge/releases/latest/download/remote-bridge_<version>_armhf.deb

# Install
sudo dpkg -i remote-bridge_<version>_armhf.deb
```

After install, edit the configuration file and restart:
```bash
sudo nano /etc/remote-bridge.conf
sudo systemctl restart remote-bridge
```

View logs:
```bash
sudo journalctl -u remote-bridge -f
```

### Alpine Linux

```bash
# Download the package for your architecture (e.g. armhf for Pi Zero v1/v2)
wget https://github.com/foxy82/pi-zero-remote-bridge/releases/latest/download/remote-bridge_<version>_armhf.apk

# Install (packages are not signed, so --allow-untrusted is required)
sudo apk add --allow-untrusted remote-bridge_<version>_armhf.apk
```

After install, edit the configuration file and restart:
```bash
sudo nano /etc/remote-bridge.conf
sudo rc-service remote-bridge restart
```

View logs:
```bash
tail -f /var/log/remote-bridge.log
```

---

## Building from Source

### 1. Build the Application
```bash
make
```

### 2. Find Your Remote Name
Run evtest to find your IR receiver's exact name:
```bash
sudo evtest
```
Look for your IR receiver in the list and copy the exact name.

### 3. Test Manually (Optional)
You can run the binary directly to test it with verbose logging:
```bash
./remote_bridge "Your Remote Name" 192.168.1.100 9999 --debug
```

### 4. Configure
Edit the configuration file and set your remote name, server IP, and port:
```bash
nano remote-bridge.conf
```

**Important**: Make sure to set all three values correctly:
- REMOTE_NAME: The exact name from evtest (in quotes)
- SERVER_IP: Your server's IP address
- SERVER_PORT: Your server's port number

### 4. Install
**Copy the config file first, then install:**
```bash
sudo cp remote-bridge.conf /etc/remote-bridge.conf
make install
```

This will:
- Install the binary to `/usr/local/bin/`
- Create a dedicated `remote-bridge` system user (no login, member of `input` group)
- Install and start the service (systemd on Debian/Raspberry Pi OS, OpenRC on Alpine)

---

## Managing the Service

### Check Status

**Debian / Raspberry Pi OS:**
```bash
sudo systemctl status remote-bridge
```

**Alpine:**
```bash
sudo rc-service remote-bridge status
```

### View Logs

**Debian / Raspberry Pi OS:**
```bash
# View recent logs
sudo journalctl -u remote-bridge -n 50

# Follow logs in real-time
sudo journalctl -u remote-bridge -f

# View logs since last boot
sudo journalctl -u remote-bridge -b
```

**Alpine:**
```bash
tail -f /var/log/remote-bridge.log
```

### Restart the Service

**Debian / Raspberry Pi OS:**
```bash
sudo systemctl restart remote-bridge
```

**Alpine:**
```bash
sudo rc-service remote-bridge restart
```

### Stop / Disable Auto-Start

**Debian / Raspberry Pi OS:**
```bash
sudo systemctl stop remote-bridge
sudo systemctl disable remote-bridge
```

**Alpine:**
```bash
sudo rc-service remote-bridge stop
sudo rc-update del remote-bridge default
```

---

## Bluetooth Pairing Web Interface

The project includes a simple web interface to help you pair Bluetooth remotes without needing to use the command line directly.

### Features
- **Scan Toggle**: Turn Bluetooth scanning on and off to discover new devices.
- **Device List**: See a list of all discovered Bluetooth devices.
- **Pair & Trust**: One-click pairing, trusting, and connecting to a device.

### Accessing the Interface
By default, the interface runs on port **8080**. You can access it by navigating to your Pi's IP address in a web browser:
`http://<your-pi-ip>:8080`

### Managing the Web Service (systemd)
The web interface is managed by the `bluetooth-pair-web` service.

**Check Status:**
```bash
sudo systemctl status bluetooth-pair-web
```

**Restart:**
```bash
sudo systemctl restart bluetooth-pair-web
```

**View Logs:**
```bash
sudo journalctl -u bluetooth-pair-web -f
```

**Stop / Disable:**
```bash
sudo systemctl stop bluetooth-pair-web
sudo systemctl disable bluetooth-pair-web
```

*Note: This service requires `python3` and `bluez` (which includes `bluetoothctl`) to be installed on your system.*

---

## Troubleshooting

### Debug Mode
Run the binary manually with `--debug` to see detailed logs about device discovery and event processing:
```bash
sudo /usr/local/bin/remote_bridge "Remote Name" <SERVER_IP> <SERVER_PORT> --debug
```

### Common Issues on Alpine Linux

1.  **Architecture Mismatch**: Pi Zero v1.x uses ARMv6. If you use a standard `armhf` package from a newer distro (like Ubuntu), it might target ARMv7 and fail. The release assets in this repo include a specific `armhf` binary built for ARMv6 via QEMU.
2.  **Permissions**: Ensure the user running the bridge has read access to `/dev/input/event*`. The service uses the `input` group. On Alpine, verify the `input` group exists and has permissions:
    ```bash
    ls -l /dev/input
    ```
    If you see nodes owned by `root:root` with `0600`, you need to set up a rule.
3.  **udev vs mdev**: Minimal Alpine installations use `mdev` instead of `udev`. The provided `udev` rule won't work in this case. You can either:
    - Install `eudev`: `apk add eudev && setup-devd udev`
    - Or add an `mdev` rule to `/etc/mdev.conf`:
      ```
      event[0-9]+ root:input 660
      ```
      Then restart `mdev` (or just reboot).
4.  **Device Name**: The `REMOTE_NAME` must match the output of `evtest` *exactly*.
5.  **UDP Connectivity**: Use `nc -u -l -p <PORT>` on your server to verify packets are arriving.

---

## Uninstall

**Debian / Raspberry Pi OS:**
```bash
sudo dpkg -r remote-bridge
sudo rm -f /etc/remote-bridge.conf
```

**Alpine:**
```bash
sudo apk del remote-bridge
sudo rm -f /etc/remote-bridge.conf
```

**From source:**
```bash
make uninstall
sudo rm -f /etc/remote-bridge.conf
```

## Configuration File Location
`/etc/remote-bridge.conf`
