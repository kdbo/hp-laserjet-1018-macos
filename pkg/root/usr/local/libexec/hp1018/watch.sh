```bash
#!/bin/bash

FIRMWARE='/usr/local/share/hp1018/sihp1018.dl'

firmware_loaded=0

get_device_uri() {
    /usr/libexec/cups/backend/usb |
        awk '$1 == "direct" && $2 ~ /^usb:\/\/Hewlett-Packard\/HP%20LaserJet%201018/ {
            print $2
            exit
        }'
}

while true; do

    DEVICE_URI="$(get_device_uri)"

    if [ -n "$DEVICE_URI" ]; then

        if [ "$firmware_loaded" -eq 0 ]; then

            if DEVICE_URI="$DEVICE_URI" \
                /usr/libexec/cups/backend/usb \
                999 root HP1018-firmware 1 "" "$FIRMWARE"
            then
                firmware_loaded=1
            fi

        fi

    else
        firmware_loaded=0
    fi

    sleep 10
done
```
