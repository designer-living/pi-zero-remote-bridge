# Makefile for remote_bridge

CC = gcc
CFLAGS = -O3 -Wall -Wextra
TARGET = remote_bridge
SOURCE = remote_bridge.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing remote_bridge..."
	sudo install -m 755 $(TARGET) /usr/local/bin/
	@echo "Creating dedicated user..."
	(id -u remote-bridge >/dev/null 2>&1 || sudo useradd -r -s /usr/sbin/nologin -d /nonexistent remote-bridge) && sudo usermod -aG input remote-bridge
	@echo "Installing systemd service..."
	sudo install -m 644 remote-bridge.service /etc/systemd/system/
	sudo systemctl daemon-reload
	@echo "Installing configuration file..."
	if [ ! -f /etc/remote-bridge.conf ]; then sudo install -m 644 remote-bridge.conf /etc/remote-bridge.conf; else echo "Configuration file /etc/remote-bridge.conf already exists, skipping."; fi
	@echo ""
	@echo "Installation complete! Enabling and starting service..."
	sudo systemctl enable remote-bridge
	sudo systemctl start remote-bridge
	@echo ""
	@echo "Service 'remote-bridge' has been enabled and started."
	@echo "If necessary, configure /etc/remote-bridge.conf and then restart the service:"
	@echo "  sudo systemctl restart remote-bridge"
	@echo "To follow logs:"
	@echo "  sudo journalctl -u remote-bridge -f"

uninstall:
	@echo "Stopping and disabling service..."
	-sudo systemctl stop remote-bridge
	-sudo systemctl disable remote-bridge
	sudo rm -f /etc/systemd/system/remote-bridge.service
	sudo rm -f /usr/local/bin/$(TARGET)
	sudo systemctl daemon-reload
	@echo "Removing dedicated user..."
	-id -u remote-bridge >/dev/null 2>&1 && sudo userdel remote-bridge
	@echo ""
	@echo "Note: The configuration file at /etc/remote-bridge.conf was not removed."
	@echo "To remove it, run: sudo rm /etc/remote-bridge.conf"
	@echo "Uninstall complete!"
