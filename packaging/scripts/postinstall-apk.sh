#!/bin/sh
set -e

# Create input group if it doesn't exist
addgroup -S input 2>/dev/null || true

# Create dedicated user if it doesn't exist
if ! id -u remote-bridge > /dev/null 2>&1; then
    adduser -S -D -H -G input -s /sbin/nologin remote-bridge
fi

# Enable and start service
rc-update add remote-bridge default
rc-service remote-bridge start || true

echo ""
echo "remote-bridge installed and started."
echo "Edit /etc/remote-bridge.conf and restart if needed:"
echo "  sudo rc-service remote-bridge restart"
echo ""
echo "Note for Alpine users: If you are not using eudev (udev),"
echo "ensure that /dev/input/event* nodes belong to the 'input' group."
echo "You can add a rule to /etc/mdev.conf if needed:"
echo "  event[0-9]+ root:input 660"
echo ""
echo "Follow logs:"
echo "  tail -f /var/log/remote-bridge.log"
