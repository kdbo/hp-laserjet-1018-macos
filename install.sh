#!/bin/sh
#
# HP LaserJet 1018 — macOS CUPS driver installer
#
# Builds and installs the HP LaserJet 1018 raster filter,
# PPD and firmware. Supports both Intel (x86_64) and
# Apple Silicon (arm64) Macs.
#
# No Ghostscript required — uses macOS's built-in cgpdftoraster.
#
# Usage:
#   sudo ./install.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

FILTER_DIR="/usr/libexec/cups/filter"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
FIRMWARE_DIR="/usr/local/share/hp1018"

FILTER="$SCRIPT_DIR/rastertozjs"
FILTER_SOURCE="$SCRIPT_DIR/rastertozjs.c"

FOO2ZJS_DIR="$SCRIPT_DIR/foo2zjs"
FIRMWARE_IMAGE="$FOO2ZJS_DIR/sihp1018.img"
FIRMWARE_TOOL="$FOO2ZJS_DIR/arm2hpdl"
FIRMWARE_TOOL_SOURCE="$FOO2ZJS_DIR/arm2hpdl.c"
FIRMWARE="$FOO2ZJS_DIR/sihp1018.dl"

PPD="$SCRIPT_DIR/PPD/HP-LaserJet_1018-native.ppd"

echo "=== HP LaserJet 1018 — CUPS Driver Installer ==="
echo ""

# ----------------------------------------------------------------------
# Check root
# ----------------------------------------------------------------------

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root."
    echo "Run:"
    echo "  sudo ./install.sh"
    exit 1
fi

# ----------------------------------------------------------------------
# Check architecture
# ----------------------------------------------------------------------

ARCH="$(uname -m)"

case "$ARCH" in
    arm64)
        echo "Detected architecture: Apple Silicon (arm64)"
        ;;
    x86_64)
        echo "Detected architecture: Intel (x86_64)"
        ;;
    *)
        echo "ERROR: Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

echo ""

# ----------------------------------------------------------------------
# Check required source files
# ----------------------------------------------------------------------

echo "[1/5] Checking source files..."

for file in \
    "$FILTER_SOURCE" \
    "$FIRMWARE_TOOL_SOURCE" \
    "$FIRMWARE_IMAGE" \
    "$PPD"
do
    if [ ! -f "$file" ]; then
        echo "ERROR: Required file not found:"
        echo "  $file"
        exit 1
    fi
done

echo "  Source files: OK"
echo ""

# ----------------------------------------------------------------------
# Build rastertozjs
# ----------------------------------------------------------------------

echo "[2/5] Building rastertozjs..."

clang \
    -o "$FILTER" \
    "$FILTER_SOURCE" \
    "$FOO2ZJS_DIR/jbig.c" \
    "$FOO2ZJS_DIR/jbig_ar.c" \
    -I"$FOO2ZJS_DIR" \
    -lcups \
    -lcupsimage \
    -Wall \
    -O2

chmod 755 "$FILTER"

echo "  rastertozjs: OK"
file "$FILTER"
echo ""

# ----------------------------------------------------------------------
# Build firmware conversion tool
# ----------------------------------------------------------------------

echo "[3/5] Preparing HP LaserJet 1018 firmware..."

if [ ! -x "$FIRMWARE_TOOL" ]; then
    clang \
        -o "$FIRMWARE_TOOL" \
        "$FIRMWARE_TOOL_SOURCE" \
        -I"$FOO2ZJS_DIR" \
        -Wall \
        -O2

    chmod 755 "$FIRMWARE_TOOL"
fi

if [ ! -f "$FIRMWARE" ]; then
    "$FIRMWARE_TOOL" "$FIRMWARE_IMAGE" > "$FIRMWARE"
fi

echo "  Firmware: $(wc -c < "$FIRMWARE" | tr -d ' ') bytes"
echo ""

# ----------------------------------------------------------------------
# Install CUPS filter
# ----------------------------------------------------------------------

echo "[4/5] Installing CUPS driver..."

mkdir -p "$FILTER_DIR"

cp "$FILTER" "$FILTER_DIR/rastertozjs"
chmod 755 "$FILTER_DIR/rastertozjs"

echo "  $FILTER_DIR/rastertozjs"

# Install the HP LaserJet 1018 PPD only.
mkdir -p "$PPD_DIR"

cp "$PPD" "$PPD_DIR/HP-LaserJet_1018-native.ppd"

echo "  $PPD_DIR/HP-LaserJet_1018-native.ppd"

# Install firmware.
mkdir -p "$FIRMWARE_DIR"

cp "$FIRMWARE" "$FIRMWARE_DIR/sihp1018.dl"

echo "  $FIRMWARE_DIR/sihp1018.dl"

echo ""

# ----------------------------------------------------------------------
# Restart CUPS
# ----------------------------------------------------------------------

echo "[5/5] Restarting CUPS..."

launchctl stop org.cups.cupsd 2>/dev/null || true
launchctl start org.cups.cupsd 2>/dev/null || true

echo ""

echo "=== Installation complete ==="
echo ""
echo "HP LaserJet 1018 driver installed."
echo ""
echo "Next steps:"
echo "  1. Connect the HP LaserJet 1018 via USB."
echo "  2. Open System Settings > Printers & Scanners."
echo "  3. Add the HP LaserJet 1018."
echo "  4. Select the HP LaserJet 1018 PPD."
echo ""