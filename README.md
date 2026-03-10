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

## Pairing a Bluetooth Remote

Use the included `pair-remote.sh` script to scan for, pair, and identify your remote's
evdev name in one step.

```bash
chmod +x pair-remote.sh
sudo ./pair-remote.sh
```

The script will:
1. Scan for Bluetooth devices (put your remote in pairing mode first)
2. Let you pick the device from a numbered list
3. Pair, trust, and connect to it
4. Detect the evdev name that appears in `/dev/input`
5. Optionally update `/etc/remote-bridge.conf` and restart the service

Requires the `bluetooth` package (`sudo apt-get install bluetooth` on Debian,
`sudo apk add bluez` on Alpine).

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

### 3. Configure
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

## Troubleshooting

### Service won't start
1. Check the configuration file:
   ```bash
   cat /etc/remote-bridge.conf
   ```

2. Verify the binary exists:
   ```bash
   ls -l /usr/local/bin/remote_bridge
   ```

3. Check for errors in the logs (see [View Logs](#view-logs) above).

### Remote not detected
1. Verify your remote is connected:
   ```bash
   sudo evtest
   ```

2. Check that the REMOTE_NAME in `/etc/remote-bridge.conf` matches exactly (case-sensitive)

3. Check the logs to see what's happening.

### Network issues
1. Verify server IP and port are correct in `/etc/remote-bridge.conf`

2. Test network connectivity:
   ```bash
   ping <SERVER_IP>
   ```

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
