#!/bin/sh
set -e

# Create input group if it doesn't exist
getent group input > /dev/null 2>&1 || groupadd -r input

# Create dedicated user if it doesn't exist
if ! id -u remote-bridge > /dev/null 2>&1; then
    useradd -r -s /usr/sbin/nologin -d /nonexistent -M remote-bridge
fi
usermod -aG input remote-bridge

# Reload udev rules to pick up the new rule if udev is present
if command -v udevadm > /dev/null 2>&1; then
    (udevadm control --reload-rules && udevadm trigger --subsystem-match=input) || echo "Warning: Failed to reload udev rules. A reboot may be required for device permissions to apply."
fi

# Reload systemd and enable/restart the service (restart handles upgrades better than start)
systemctl daemon-reload
systemctl enable remote-bridge
systemctl restart remote-bridge || true

echo ""
echo "remote-bridge installed and started."
echo "Edit /etc/remote-bridge.conf and restart if needed:"
echo "  sudo systemctl restart remote-bridge"
echo "Follow logs:"
echo "  sudo journalctl -u remote-bridge -f"
