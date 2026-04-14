#!/bin/bash
# ============================================
# FieldLink Raspberry Pi 3 Setup Script
# ============================================
# Run on a fresh Raspberry Pi OS (Lite or Desktop):
#   curl -sL https://raw.githubusercontent.com/Voltageza/fieldlink/main/tools/pi-setup.sh | bash
#   OR
#   chmod +x pi-setup.sh && ./pi-setup.sh
#
# What this script does:
#   1. System update
#   2. Python 3 + pip + paho-mqtt
#   3. PlatformIO Core CLI (for building & flashing firmware)
#   4. Tailscale (secure remote access from anywhere)
#   5. screen (serial monitor)
#   6. USB serial permissions (dialout group)
#   7. Clone FieldLink repo
#   8. Helper scripts in ~/fieldlink-tools/
#
# After running, reboot and run: tailscale up
# ============================================

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[SETUP]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

REPO_URL="https://github.com/Voltageza/fieldlink.git"
INSTALL_DIR="$HOME/fieldlink"
TOOLS_DIR="$HOME/fieldlink-tools"

# ============================================
# 1. System Update
# ============================================
log "Updating system packages..."
sudo apt-get update -qq
sudo apt-get upgrade -y -qq

# ============================================
# 2. Python 3 + pip + dependencies
# ============================================
log "Installing Python 3 and dependencies..."
sudo apt-get install -y -qq python3 python3-pip python3-venv git screen minicom usbutils

log "Installing Python packages..."
pip3 install --user paho-mqtt requests

# ============================================
# 3. PlatformIO Core CLI
# ============================================
log "Installing PlatformIO Core CLI..."
if command -v pio &> /dev/null; then
    log "PlatformIO already installed"
else
    curl -fsSL -o /tmp/get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
    python3 /tmp/get-platformio.py

    # Add to PATH
    if ! grep -q "platformio" "$HOME/.profile" 2>/dev/null; then
        echo '' >> "$HOME/.profile"
        echo '# PlatformIO Core' >> "$HOME/.profile"
        echo 'export PATH="$PATH:$HOME/.platformio/penv/bin"' >> "$HOME/.profile"
    fi
    export PATH="$PATH:$HOME/.platformio/penv/bin"
fi

# ============================================
# 4. Tailscale (secure remote access)
# ============================================
log "Installing Tailscale..."
if command -v tailscale &> /dev/null; then
    log "Tailscale already installed"
else
    curl -fsSL https://tailscale.com/install.sh | sh
fi

# ============================================
# 5. USB Serial Permissions
# ============================================
log "Setting up USB serial permissions..."
sudo usermod -aG dialout $USER

# udev rule for ESP32-S3 (Espressif USB VID)
sudo tee /etc/udev/rules.d/99-esp32.rules > /dev/null << 'UDEV'
# ESP32-S3 USB-Serial/JTAG
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", MODE="0666", SYMLINK+="esp32_eve"
# CP2102/CH340 USB-Serial adapters
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", MODE="0666"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"
UDEV
sudo udevadm control --reload-rules

# ============================================
# 6. Clone FieldLink Repo
# ============================================
log "Cloning FieldLink repository..."
if [ -d "$INSTALL_DIR" ]; then
    log "Repo already exists, pulling latest..."
    cd "$INSTALL_DIR" && git pull
else
    git clone "$REPO_URL" "$INSTALL_DIR"
fi

# ============================================
# 7. Create Helper Scripts
# ============================================
log "Creating helper scripts in $TOOLS_DIR..."
mkdir -p "$TOOLS_DIR"

# --- fl-monitor: Serial monitor ---
cat > "$TOOLS_DIR/fl-monitor" << 'SCRIPT'
#!/bin/bash
# Serial monitor for ESP32 device
# Usage: fl-monitor [port]
PORT=${1:-/dev/ttyACM0}
BAUD=115200

if [ ! -e "$PORT" ]; then
    echo "Port $PORT not found. Available ports:"
    ls /dev/ttyACM* /dev/ttyUSB* /dev/esp32_eve 2>/dev/null || echo "  No USB serial devices found"
    exit 1
fi

echo "Connecting to $PORT at $BAUD baud..."
echo "Press Ctrl+A then K to exit"
screen $PORT $BAUD
SCRIPT

# --- fl-flash: Flash firmware ---
cat > "$TOOLS_DIR/fl-flash" << 'SCRIPT'
#!/bin/bash
# Flash Eve firmware to connected device
# Usage: fl-flash [port]
PORT=${1:-/dev/ttyACM0}
DIR="$HOME/fieldlink/Main Code/projects/eve-controller"

if [ ! -e "$PORT" ]; then
    echo "Port $PORT not found. Available ports:"
    ls /dev/ttyACM* /dev/ttyUSB* /dev/esp32_eve 2>/dev/null || echo "  No USB serial devices found"
    exit 1
fi

echo "Building and flashing Eve firmware to $PORT..."
cd "$DIR"
~/.platformio/penv/bin/pio run -e esp32-s3 -t upload --upload-port "$PORT"
SCRIPT

# --- fl-build: Build only ---
cat > "$TOOLS_DIR/fl-build" << 'SCRIPT'
#!/bin/bash
# Build Eve firmware without flashing
DIR="$HOME/fieldlink/Main Code/projects/eve-controller"
echo "Building Eve firmware..."
cd "$DIR"
~/.platformio/penv/bin/pio run -e esp32-s3
SCRIPT

# --- fl-check: Device checklist ---
cat > "$TOOLS_DIR/fl-check" << 'SCRIPT'
#!/bin/bash
# Run device checklist
# Usage: fl-check [device_id | --all]
cd "$HOME/fieldlink"
python3 tools/device_checklist.py "$@"
SCRIPT

# --- fl-test: I/O test tool ---
cat > "$TOOLS_DIR/fl-test" << 'SCRIPT'
#!/bin/bash
# Run I/O test tool (requires feature/io-test firmware)
# Usage: fl-test [device_id]
cd "$HOME/fieldlink"
python3 tools/io_test.py "$@"
SCRIPT

# --- fl-ota: OTA firmware update ---
cat > "$TOOLS_DIR/fl-ota" << 'SCRIPT'
#!/bin/bash
# Send OTA update command to device
# Usage: fl-ota <device_id> <version>
if [ $# -ne 2 ]; then
    echo "Usage: fl-ota <device_id> <version>"
    echo "Example: fl-ota FL-CC8CA0 1.2.3"
    exit 1
fi
cd "$HOME/fieldlink"
python3 mqtt_ota_eve.py "$1" "$2"
SCRIPT

# --- fl-pull: Update repo ---
cat > "$TOOLS_DIR/fl-pull" << 'SCRIPT'
#!/bin/bash
# Pull latest code from GitHub
cd "$HOME/fieldlink"
git pull
echo "Updated to: $(git log --oneline -1)"
SCRIPT

# --- fl-ports: List serial ports ---
cat > "$TOOLS_DIR/fl-ports" << 'SCRIPT'
#!/bin/bash
# List connected USB serial devices
echo "USB Serial Devices:"
echo "==================="
for port in /dev/ttyACM* /dev/ttyUSB*; do
    if [ -e "$port" ]; then
        info=$(udevadm info -q property "$port" 2>/dev/null | grep -E "ID_SERIAL=|ID_MODEL=" | head -2)
        echo "  $port"
        echo "    $info" | sed 's/^/    /'
    fi
done

if [ -L /dev/esp32_eve ]; then
    echo ""
    echo "Symlink: /dev/esp32_eve -> $(readlink -f /dev/esp32_eve)"
fi
SCRIPT

# Make all scripts executable
chmod +x "$TOOLS_DIR"/*

# Add tools to PATH
if ! grep -q "fieldlink-tools" "$HOME/.profile" 2>/dev/null; then
    echo '' >> "$HOME/.profile"
    echo '# FieldLink tools' >> "$HOME/.profile"
    echo 'export PATH="$PATH:$HOME/fieldlink-tools"' >> "$HOME/.profile"
fi

# ============================================
# 8. Summary
# ============================================
echo ""
echo "============================================"
echo "  FieldLink Pi Setup Complete!"
echo "============================================"
echo ""
echo "  Available commands (after reboot):"
echo "    fl-check --all     Check all devices"
echo "    fl-check FL-22F968 Check specific device"
echo "    fl-monitor         Serial monitor (Ctrl+A K to exit)"
echo "    fl-flash           Build & flash via USB"
echo "    fl-build           Build firmware only"
echo "    fl-test            I/O test tool"
echo "    fl-ota ID VER      OTA update via MQTT"
echo "    fl-pull            Update code from GitHub"
echo "    fl-ports           List USB serial devices"
echo ""
echo "  Next steps:"
echo "    1. Reboot:  sudo reboot"
echo "    2. Connect to Tailscale:  sudo tailscale up"
echo "    3. Plug in Eve device via USB"
echo "    4. Test:  fl-ports && fl-check --all"
echo ""
echo "  Tailscale: After 'tailscale up', you'll get a"
echo "  login URL. Open it on your phone/PC to authorize."
echo "  Then SSH from anywhere: ssh pi@<tailscale-ip>"
echo "============================================"
