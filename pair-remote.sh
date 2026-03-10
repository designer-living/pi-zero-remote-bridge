#!/bin/sh
# pair-remote.sh - Pair a Bluetooth remote and get its evdev name for remote-bridge
set -e

CONFIG_FILE="/etc/remote-bridge.conf"
SCAN_SECONDS=15

# --- Colours (only when stdout is a terminal) ---
if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; BOLD=''; NC=''
fi

info()   { printf "${BLUE}→${NC} %s\n" "$*"; }
ok()     { printf "${GREEN}✓${NC} %s\n" "$*"; }
warn()   { printf "${YELLOW}⚠${NC} %s\n" "$*"; }
err()    { printf "${RED}✗${NC} %s\n" "$*" >&2; }
header() { printf "\n${BOLD}%s${NC}\n" "$*"; }
die()    { err "$*"; exit 1; }

# --- Dependency check ---
command -v bluetoothctl >/dev/null 2>&1 \
    || die "bluetoothctl not found. Install: apt-get install bluetooth  OR  apk add bluez"

bluetoothctl show >/dev/null 2>&1 \
    || die "Cannot access Bluetooth. Try running with sudo."

# --- Power on ---
header "Setting up Bluetooth..."
bluetoothctl power on >/dev/null 2>&1 \
    && ok "Adapter powered on" \
    || die "Failed to power on Bluetooth adapter"

# --- Snapshot existing input devices so we can detect the new one later ---
EXISTING_DEVS=$(ls /sys/class/input/ 2>/dev/null | grep '^event' || true)

# --- Scan ---
header "Scanning for ${SCAN_SECONDS} seconds..."
info "Put your remote into pairing mode now."
echo ""

bluetoothctl scan on >/dev/null 2>&1
sleep "$SCAN_SECONDS"
bluetoothctl scan off >/dev/null 2>&1 || true
sleep 1

# Collect discovered devices as "MAC Name" lines
DEVICES=$(bluetoothctl devices | awk '{mac=$2; $1=$2=""; name=$0; gsub(/^ +/,"",name); print mac " " name}')

if [ -z "$DEVICES" ]; then
    die "No devices found. Make sure your remote is in pairing mode and try again."
fi

DEVICE_COUNT=$(echo "$DEVICES" | wc -l)

# --- Device selection menu ---
header "Discovered devices:"
echo ""

i=1
while IFS= read -r line; do
    mac=$(echo "$line" | awk '{print $1}')
    name=$(echo "$line" | cut -d' ' -f2-)
    printf "  ${BOLD}%2d)${NC}  %s  %s\n" "$i" "$mac" "$name"
    i=$((i+1))
done << EOF
$DEVICES
EOF

echo ""
printf "Select device [1-%d]: " "$DEVICE_COUNT"
read -r CHOICE

# Validate: must be a number within range
case "$CHOICE" in
    ''|*[!0-9]*) die "Invalid selection." ;;
esac
if [ "$CHOICE" -lt 1 ] || [ "$CHOICE" -gt "$DEVICE_COUNT" ]; then
    die "Invalid selection."
fi

SELECTED_LINE=$(echo "$DEVICES" | sed -n "${CHOICE}p")
SELECTED_MAC=$(echo "$SELECTED_LINE" | awk '{print $1}')
SELECTED_NAME=$(echo "$SELECTED_LINE" | cut -d' ' -f2-)

# --- Pair ---
header "Pairing with ${SELECTED_NAME} (${SELECTED_MAC})..."

PAIR_OUTPUT=$(bluetoothctl pair "$SELECTED_MAC" 2>&1 || true)
if echo "$PAIR_OUTPUT" | grep -qi "failed\|not available\|error"; then
    warn "Pairing response: $PAIR_OUTPUT"
    warn "Continuing anyway — device may already be paired."
else
    ok "Paired"
fi

bluetoothctl trust "$SELECTED_MAC" >/dev/null 2>&1 \
    && ok "Trusted (device will auto-connect on boot)"

# --- Connect ---
info "Connecting..."
CONNECT_OUTPUT=$(bluetoothctl connect "$SELECTED_MAC" 2>&1 || true)
if echo "$CONNECT_OUTPUT" | grep -qi "successful\|already connected"; then
    ok "Connected"
else
    warn "Could not confirm connection — the device may connect automatically."
fi

# --- Wait for a new evdev device to appear ---
header "Waiting for input device to appear (up to 15 seconds)..."

EVDEV_NAME=""
attempt=1
while [ "$attempt" -le 15 ]; do
    sleep 1
    NEW_DEVS=$(ls /sys/class/input/ 2>/dev/null | grep '^event' || true)
    for dev in $NEW_DEVS; do
        if ! echo "$EXISTING_DEVS" | grep -q "^${dev}$"; then
            DEV_NAME=$(cat "/sys/class/input/${dev}/device/name" 2>/dev/null || true)
            if [ -n "$DEV_NAME" ]; then
                EVDEV_NAME="$DEV_NAME"
                ok "Detected input device: ${BOLD}${EVDEV_NAME}${NC}"
                break 2
            fi
        fi
    done
    printf "."
    attempt=$((attempt+1))
done
echo ""

if [ -z "$EVDEV_NAME" ]; then
    warn "Could not detect the evdev name automatically."
    warn "Once connected, run 'sudo evtest' to find the device name manually."
    exit 0
fi

# --- Show result ---
header "Add this to ${CONFIG_FILE}:"
echo ""
printf "  ${BOLD}REMOTE_NAME=\"%s\"${NC}\n" "$EVDEV_NAME"
echo ""

# --- Optionally update config ---
if [ -f "$CONFIG_FILE" ]; then
    printf "Update %s with this name now? [y/N]: " "$CONFIG_FILE"
    read -r UPDATE
    case "$UPDATE" in
        [Yy]*)
            sudo sed -i "s|^REMOTE_NAME=.*|REMOTE_NAME=\"${EVDEV_NAME}\"|" "$CONFIG_FILE"
            ok "Updated ${CONFIG_FILE}"

            # Offer service restart
            if command -v systemctl >/dev/null 2>&1 && systemctl is-active remote-bridge >/dev/null 2>&1; then
                printf "Restart remote-bridge service? [y/N]: "
                read -r RESTART
                case "$RESTART" in
                    [Yy]*) sudo systemctl restart remote-bridge && ok "Service restarted" ;;
                esac
            elif command -v rc-service >/dev/null 2>&1 && rc-service remote-bridge status >/dev/null 2>&1; then
                printf "Restart remote-bridge service? [y/N]: "
                read -r RESTART
                case "$RESTART" in
                    [Yy]*) sudo rc-service remote-bridge restart && ok "Service restarted" ;;
                esac
            fi
            ;;
    esac
fi

echo ""
ok "Done! Your remote is paired, trusted, and will reconnect automatically."
