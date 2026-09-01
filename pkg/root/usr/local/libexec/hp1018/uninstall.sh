#!/bin/bash

set -e

PRODUCT="HP LaserJet 1018"

FILTER="/usr/libexec/cups/filter/rastertozjs"
HP1018_DIR="/usr/local/libexec/hp1018"
PPD="/Library/Printers/PPDs/Contents/Resources/HP-LaserJet_1018-native.ppd"
SHARE_DIR="/usr/local/share/hp1018"
LAUNCHD_FIRMWARE="/Library/LaunchDaemons/com.kdbo.hp1018-firmware.plist"
LAUNCHD_AIRPRINT="/Library/LaunchDaemons/com.kdbo.hp1018.airprint.plist"
APP_SUPPORT="/Library/Application Support/HP1018"
UNINSTALLER="/usr/local/bin/hp1018-uninstall"
STATE_DIR="/var/run/hp1018"

echo "=== $PRODUCT Uninstaller ==="
echo ""

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This uninstaller must be run as root."
    echo ""
    echo "Run:"
    echo "  sudo $UNINSTALLER"
    exit 1
fi

echo "Stopping HP1018 services..."

if [ -f "$LAUNCHD_FIRMWARE" ]; then
    launchctl bootout system "$LAUNCHD_FIRMWARE" 2>/dev/null || true
fi

if [ -f "$LAUNCHD_AIRPRINT" ]; then
    launchctl bootout system "$LAUNCHD_AIRPRINT" 2>/dev/null || true
fi

echo "Removing HP1018 files..."

rm -f "$FILTER"
rm -rf "$HP1018_DIR"
rm -rf "$SHARE_DIR"
rm -f "$LAUNCHD_FIRMWARE"
rm -f "$LAUNCHD_AIRPRINT"
rm -rf "$APP_SUPPORT"
rm -rf "$PPD"
rm -rf "$STATE_DIR"

echo "Restarting CUPS..."

launchctl kickstart -k system/org.cups.cupsd 2>/dev/null || true

# Remove the uninstall command itself last.
rm -f "$UNINSTALLER"

echo ""
echo "=== $PRODUCT has been uninstalled ==="
echo ""
