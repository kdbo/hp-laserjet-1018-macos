#!/bin/bash

DEVICE_URI='usb://Hewlett-Packard/HP%20LaserJet%201018?serial=KP1N5K4'
FIRMWARE='/usr/local/share/hp1018/sihp1018.dl'
STATE='/tmp/hp1018-firmware-loaded'

if /usr/libexec/cups/backend/usb | grep -Fq "$DEVICE_URI"; then
    if [ ! -f "$STATE" ]; then
        DEVICE_URI="$DEVICE_URI" \
        /usr/libexec/cups/backend/usb \
        999 root HP1018-firmware 1 "" "$FIRMWARE"

        if [ $? -eq 0 ]; then
            touch "$STATE"
        fi
    fi
else
    rm -f "$STATE"
fi
