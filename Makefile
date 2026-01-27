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
	@echo "Installing systemd service..."
	sudo install -m 644 remote-bridge.service /etc/systemd/system/
	sudo systemctl daemon-reload
	@echo ""
	@echo "Installation complete!"
	@echo "Configure /etc/remote-bridge.conf and then run:"
	@echo "  sudo systemctl enable remote-bridge"
	@echo "  sudo systemctl start remote-bridge"

uninstall:
	@echo "Stopping and disabling service..."
	-sudo systemctl stop remote-bridge
	-sudo systemctl disable remote-bridge
	sudo rm -f /etc/systemd/system/remote-bridge.service
	sudo rm -f /usr/local/bin/$(TARGET)
	sudo systemctl daemon-reload
	@echo "Uninstall complete!"
