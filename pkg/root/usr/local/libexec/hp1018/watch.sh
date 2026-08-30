#!/bin/bash

FIRMWARE='/usr/local/share/hp1018/sihp1018.dl'
STATE_DIR='/var/run/hp1018'

ensure_state_dir() {
    /bin/mkdir -p "$STATE_DIR"
    /bin/chmod 700 "$STATE_DIR"
}

state_key() {
    /sbin/md5 -q -s "$1"
}

# Output every CUPS queue configured for a HP LaserJet 1018 together with its
# complete DeviceURI. The URI, including its serial query parameter when
# present, identifies the physical printer.
list_hp1018_queues() {
    /usr/bin/lpstat -v 2>/dev/null |
        /usr/bin/awk '
            $1 == "device" && $2 == "for" {
                queue = $3
                sub(/:$/, "", queue)
                uri = $4

                if (uri ~ /^usb:\/\/Hewlett-Packard\/HP%20LaserJet%201018(\?|$)/)
                    print queue "\t" uri
            }
        '
}

is_device_connected() {
    local device_uri="$1"

    /usr/libexec/cups/backend/usb |
        /usr/bin/awk -v uri="$device_uri" '
            $1 == "direct" && $2 == uri {
                found = 1
            }

            END {
                exit !found
            }
        '
}

remove_disconnected_firmware_states() {
    local state_file device_uri

    for state_file in "$STATE_DIR"/firmware-*; do
        [ -f "$state_file" ] || continue

        device_uri="$(/bin/cat "$state_file")"
        if ! is_device_connected "$device_uri"; then
            /bin/rm -f "$state_file"
        fi
    done
}

load_firmware_if_needed() {
    local queue="$1"
    local device_uri="$2"
    local state_file

    state_file="$STATE_DIR/firmware-$(state_key "$device_uri")"

    [ -f "$state_file" ] && return

    if DEVICE_URI="$device_uri" \
        /usr/libexec/cups/backend/usb \
        999 root "HP1018-firmware-$queue" 1 "" "$FIRMWARE"
    then
        printf '%s\n' "$device_uri" > "$state_file"
    fi
}

ensure_state_dir

while true; do
    remove_disconnected_firmware_states

    while IFS=$'\t' read -r queue device_uri; do
        [ -n "$queue" ] && [ -n "$device_uri" ] || continue

        if is_device_connected "$device_uri"; then
            load_firmware_if_needed "$queue" "$device_uri"
        fi
    done < <(list_hp1018_queues)

    sleep 10
done
