#!/bin/sh
set -e

# Create input group if it doesn't exist
addgroup -S input 2>/dev/null || true

# Create dedicated user if it doesn't exist
if ! id -u remote-bridge > /dev/null 2>&1; then
    adduser -S -D -H -G input -s /sbin/nologin remote-bridge
fi

# Reload udev rules to pick up the new rule if udev is present
if command -v udevadm > /dev/null 2>&1; then
    (udevadm control --reload-rules && udevadm trigger --subsystem-match=input) || echo "Warning: Failed to reload udev rules. A reboot may be required for device permissions to apply."
fi

# Enable and restart service (restart handles upgrades better than start)
rc-update add remote-bridge default
rc-service remote-bridge restart || true

echo ""
echo "remote-bridge installed and started."
echo "Edit /etc/remote-bridge.conf and restart if needed:"
echo "  sudo rc-service remote-bridge restart"
echo "Follow logs:"
echo "  tail -f /var/log/remote-bridge.log"
