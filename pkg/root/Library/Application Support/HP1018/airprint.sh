#!/bin/bash

PRINTER="HP_LaserJet_1018"
PRINTERS_CONF="/etc/cups/printers.conf"

dns_pid=""

get_printer_value() {
    local key="$1"

    /usr/bin/awk -v printer="$PRINTER" -v key="$key" '
        $0 == "<Printer " printer ">" {
            found=1
            next
        }

        found && $0 ~ "^" key " " {
            sub("^" key " ", "")
            print
            exit
        }

        found && /^<\/Printer>$/ {
            exit
        }
    ' "$PRINTERS_CONF"
}

is_shared() {
    /usr/bin/awk -v printer="$PRINTER" '
        $0 == "<Printer " printer ">" {
            found=1
            next
        }

        found && /^Shared Yes$/ {
            exit 0
        }

        found && /^<\/Printer>$/ {
            exit 1
        }

        END {
            if (!found)
                exit 1
        }
    ' "$PRINTERS_CONF"
}

stop_airprint() {
    if [ -n "$dns_pid" ]; then
        kill "$dns_pid" 2>/dev/null
        wait "$dns_pid" 2>/dev/null
        dns_pid=""
    fi
}

trap stop_airprint EXIT TERM INT

start_airprint() {
    local ty="$1"
    local note="$2"

    /usr/bin/dns-sd -R \
      "$ty AirPrint" \
      _ipp._tcp,_universal \
      local \
      631 \
      "txtvers=1" \
      "qtotal=1" \
      "rp=printers/$PRINTER" \
      "ty=$ty" \
      "product=($ty)" \
      "note=$note" \
      "pdl=application/pdf,image/urf" \
      "URF=V1.4,W8,CP1,RS600,DM1" \
      "Color=F" \
      "Duplex=F" &

    dns_pid=$!
}

while true; do

    if is_shared; then

        ty="$(get_printer_value "Info")"
        note="$(get_printer_value "Location")"

        [ -z "$ty" ] && ty="HP LaserJet 1018"
        [ -z "$note" ] && note="Koen’s Mac mini"

        if [ -z "$dns_pid" ] || ! kill -0 "$dns_pid" 2>/dev/null; then
            start_airprint "$ty" "$note"
        fi

    else

        stop_airprint
    fi

    sleep 10
done
