#!/usr/bin/env python3
import http.server
import socketserver
import subprocess
import json
import re
import os

PORT = 8080

# Try to find the web directory in common installation locations
POSSIBLE_DIRS = [
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "web"),
    "/usr/local/share/remote-bridge/web",
    "./web"
]

DIRECTORY = POSSIBLE_DIRS[0]
for d in POSSIBLE_DIRS:
    if os.path.exists(d):
        DIRECTORY = d
        break

class BluetoothHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def do_POST(self):
        if self.path == '/api/scan/on':
            self.run_bluetoothctl(['scan', 'on'])
            self.send_json({"status": "ok"})
        elif self.path == '/api/scan/off':
            self.run_bluetoothctl(['scan', 'off'])
            self.send_json({"status": "ok"})
        elif self.path.startswith('/api/pair/'):
            mac = self.path.split('/')[-1]
            success, error = self.pair_device(mac)
            if success:
                self.send_json({"status": "ok"})
            else:
                self.send_response(500)
                self.send_json({"status": "error", "error": error})
        else:
            self.send_error(404)

    def do_GET(self):
        if self.path == '/api/devices':
            devices = self.get_devices()
            self.send_json(devices)
        else:
            super().do_GET()

    def send_json(self, data):
        self.send_response(200)
        self.send_header('Content-type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def run_bluetoothctl(self, args):
        try:
            # bluetoothctl is often interactive, but simple commands work with subprocess
            subprocess.run(['bluetoothctl'] + args, check=True, capture_output=True, text=True)
            return True
        except subprocess.CalledProcessError as e:
            print(f"Error running bluetoothctl: {e.stderr}")
            return False

    def get_devices(self):
        # Get all devices found
        output = ""
        try:
            result = subprocess.run(['bluetoothctl', 'devices'], capture_output=True, text=True)
            output = result.stdout
        except Exception as e:
            print(f"Error getting devices: {e}")
            return []

        # Get paired devices to mark them
        paired_output = ""
        try:
            result = subprocess.run(['bluetoothctl', 'paired-devices'], capture_output=True, text=True)
            paired_output = result.stdout
        except:
            pass

        paired_macs = set(re.findall(r'Device ([0-9A-F:]{17})', paired_output))

        devices = []
        for line in output.splitlines():
            match = re.match(r'Device ([0-9A-F:]{17}) (.*)', line)
            if match:
                mac = match.group(1)
                name = match.group(2)
                devices.append({
                    "mac": mac,
                    "name": name,
                    "paired": mac in paired_macs
                })
        return devices

    def pair_device(self, mac):
        print(f"Pairing device {mac}...")
        try:
            # Pair
            res = subprocess.run(['bluetoothctl', 'pair', mac], capture_output=True, text=True, timeout=30)
            if res.returncode != 0:
                return False, res.stderr or res.stdout
            
            # Trust
            subprocess.run(['bluetoothctl', 'trust', mac], check=True)
            
            # Connect (best effort)
            subprocess.run(['bluetoothctl', 'connect', mac], timeout=10)
            
            return True, None
        except Exception as e:
            return False, str(e)

if __name__ == '__main__':
    if not os.path.exists(DIRECTORY):
        os.makedirs(DIRECTORY)
    
    # Ensure bluetoothctl is present
    try:
        subprocess.run(['bluetoothctl', '--version'], capture_output=True)
    except FileNotFoundError:
        print("Warning: bluetoothctl not found. This script will only work on Linux with BlueZ.")

    with socketserver.TCPServer(("", PORT), BluetoothHandler) as httpd:
        print(f"Serving Bluetooth pairing interface at http://localhost:{PORT}")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nStopping server...")
            httpd.server_close()
