# Makefile for remote_bridge

VERSION ?= dev
CC = gcc
DEFS = -DVERSION=\"$(VERSION)\"
CFLAGS ?= -O3 -Wall -Wextra
TARGET = remote_bridge
SOURCE = remote_bridge.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(DEFS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing remote_bridge..."
	sudo install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installing bluetooth_pair_web..."
	sudo install -m 755 bluetooth_pair_web.py /usr/local/bin/bluetooth-pair-web
	sudo mkdir -p /usr/local/share/remote-bridge/web
	sudo install -m 644 web/index.html /usr/local/share/remote-bridge/web/
	@echo "Installing udev rule..."
	sudo install -m 644 99-remote-bridge.rules /etc/udev/rules.d/
	@echo "Creating dedicated user..."
	@if command -v useradd >/dev/null 2>&1; then \
		id -u remote-bridge >/dev/null 2>&1 || sudo useradd -r -s /usr/sbin/nologin -d /nonexistent -M remote-bridge; \
		sudo usermod -aG input remote-bridge; \
	else \
		sudo addgroup -S input 2>/dev/null || true; \
		id -u remote-bridge >/dev/null 2>&1 || sudo adduser -S -D -H -G input -s /sbin/nologin remote-bridge; \
	fi
	@if [ ! -f /etc/remote-bridge.conf ]; then \
		echo "Installing configuration file..."; \
		sudo install -m 644 remote-bridge.conf /etc/remote-bridge.conf; \
	else \
		echo "Configuration file /etc/remote-bridge.conf already exists, skipping."; \
	fi
	@if command -v systemctl >/dev/null 2>&1 && systemctl --version >/dev/null 2>&1; then \
		echo "Installing systemd service..."; \
		sudo install -m 644 remote-bridge.service /etc/systemd/system/; \
		sudo install -m 644 bluetooth-pair-web.service /etc/systemd/system/; \
		sudo systemctl daemon-reload; \
		echo "Enabling and starting services..."; \
		sudo systemctl enable remote-bridge; \
		sudo systemctl start remote-bridge; \
		sudo systemctl enable bluetooth-pair-web; \
		sudo systemctl start bluetooth-pair-web; \
		echo ""; \
		echo "Services 'remote-bridge' and 'bluetooth-pair-web' have been enabled and started."; \
		echo "If necessary, configure /etc/remote-bridge.conf and then restart the service:"; \
		echo "  sudo systemctl restart remote-bridge"; \
		echo "To follow logs:"; \
		echo "  sudo journalctl -u remote-bridge -f"; \
	elif command -v rc-update >/dev/null 2>&1; then \
		echo "Installing OpenRC service..."; \
		sudo install -m 755 remote-bridge.initd /etc/init.d/remote-bridge; \
		echo "Enabling and starting service..."; \
		sudo rc-update add remote-bridge default; \
		sudo rc-service remote-bridge start || true; \
		echo ""; \
		echo "Service 'remote-bridge' has been enabled and started."; \
		echo "If necessary, configure /etc/remote-bridge.conf and then restart the service:"; \
		echo "  sudo rc-service remote-bridge restart"; \
		echo "To follow logs:"; \
		echo "  tail -f /var/log/remote-bridge.log"; \
	else \
		echo "WARNING: Could not detect init system (systemd or OpenRC)."; \
		echo "Please install the service manually."; \
	fi

uninstall:
	@echo "Stopping and disabling service..."
	@if command -v systemctl >/dev/null 2>&1 && systemctl --version >/dev/null 2>&1; then \
		sudo systemctl stop remote-bridge || true; \
		sudo systemctl disable remote-bridge || true; \
		sudo rm -f /etc/systemd/system/remote-bridge.service; \
		sudo systemctl daemon-reload; \
	elif command -v rc-update >/dev/null 2>&1; then \
		sudo rc-service remote-bridge stop || true; \
		sudo rc-update del remote-bridge default || true; \
		sudo rm -f /etc/init.d/remote-bridge; \
	fi
	sudo rm -f /etc/udev/rules.d/99-remote-bridge.rules
	sudo rm -f /usr/local/bin/$(TARGET)
	sudo rm -f /usr/local/bin/bluetooth-pair-web
	sudo rm -rf /usr/local/share/remote-bridge/web
	sudo rm -f /etc/systemd/system/bluetooth-pair-web.service
	@echo "Removing dedicated user..."
	@if command -v userdel >/dev/null 2>&1; then \
		id -u remote-bridge >/dev/null 2>&1 && sudo userdel remote-bridge || true; \
	elif command -v deluser >/dev/null 2>&1; then \
		id -u remote-bridge >/dev/null 2>&1 && sudo deluser remote-bridge || true; \
	fi
	@echo ""
	@echo "Note: The configuration file at /etc/remote-bridge.conf was not removed."
	@echo "To remove it, run: sudo rm /etc/remote-bridge.conf"
	@echo "Uninstall complete!"
