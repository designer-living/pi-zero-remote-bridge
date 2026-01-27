# Remote Bridge - Installation & Usage Guide

## Quick Setup

### 1. Build the Application
```bash
make
```

### 2. Configure
Edit the configuration file and set your remote name, server IP, and port:
```bash
sudo nano remote-bridge.conf
```

Find your remote name by running:
```bash
sudo evtest
```
Look for your IR receiver in the list and copy the exact name.

### 3. Install
```bash
sudo cp remote-bridge.conf /etc/
make install
```

### 4. Start the Service
```bash
sudo systemctl enable remote-bridge
sudo systemctl start remote-bridge
```

## Managing the Service

### Check Status
```bash
sudo systemctl status remote-bridge
```

### View Logs
```bash
# View recent logs
sudo journalctl -u remote-bridge -n 50

# Follow logs in real-time
sudo journalctl -u remote-bridge -f

# View logs since last boot
sudo journalctl -u remote-bridge -b

# View logs for specific time range
sudo journalctl -u remote-bridge --since "1 hour ago"
sudo journalctl -u remote-bridge --since "2024-01-01" --until "2024-01-02"
```

### Restart the Service
```bash
sudo systemctl restart remote-bridge
```

### Stop the Service
```bash
sudo systemctl stop remote-bridge
```

### Disable Auto-Start
```bash
sudo systemctl disable remote-bridge
```

## Troubleshooting

### Service won't start
1. Check the configuration file:
   ```bash
   cat /etc/remote-bridge.conf
   ```

2. Check for errors in the logs:
   ```bash
   sudo journalctl -u remote-bridge -n 50
   ```

3. Verify the binary exists:
   ```bash
   ls -l /usr/local/bin/remote_bridge
   ```

### Remote not detected
1. Verify your remote is connected:
   ```bash
   sudo evtest
   ```

2. Check that the REMOTE_NAME in `/etc/remote-bridge.conf` matches exactly (case-sensitive)

3. Check the logs to see what's happening:
   ```bash
   sudo journalctl -u remote-bridge -f
   ```

### Network issues
1. Verify server IP and port are correct in `/etc/remote-bridge.conf`

2. Test network connectivity:
   ```bash
   ping <SERVER_IP>
   ```

## Uninstall
```bash
make uninstall
sudo rm -f /etc/remote-bridge.conf
```

## Configuration File Location
`/etc/remote-bridge.conf`

## Log Location
Logs are stored in the systemd journal. Access them using:
```bash
journalctl -u remote-bridge
```

## Service Features
- **Auto-start on boot**: Enabled with `systemctl enable`
- **Auto-restart on crash**: Restarts automatically after 5 seconds
- **Centralized logging**: All logs go to systemd journal
- **Easy configuration**: Simple config file in `/etc/`
