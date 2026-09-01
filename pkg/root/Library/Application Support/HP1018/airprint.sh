#!/bin/bash

PRINTERS_CONF='/etc/cups/printers.conf'
STATE_DIR='/var/run/hp1018'

ensure_state_dir() {
    /bin/mkdir -p "$STATE_DIR"
    /bin/chmod 700 "$STATE_DIR"
}

state_key() {
    /sbin/md5 -q -s "$1"
}

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

get_queue_device_uri() {
    local printer="$1"

    /usr/bin/lpstat -v "$printer" 2>/dev/null |
        /usr/bin/awk -v printer="$printer" '
            $1 == "device" && $2 == "for" {
                queue = $3
                sub(/:$/, "", queue)

                if (queue == printer) {
                    print $4
                    exit
                }
            }
        '
}

queue_is_configured_for_uri() {
    [ "$(get_queue_device_uri "$1")" = "$2" ]
}

is_printer_available() {
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

get_printer_value() {
    local printer="$1"
    local key="$2"

    /usr/bin/awk -v printer="$printer" -v key="$key" '
        $0 == "<Printer " printer ">" {
            found = 1
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
    local printer="$1"

    /usr/bin/awk -v printer="$printer" '
        $0 == "<Printer " printer ">" {
            found = 1
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

pid_file_for_queue() {
    printf '%s/airprint-%s.pid\n' "$STATE_DIR" "$(state_key "$1")"
}

identity_file_for_queue() {
    printf '%s/airprint-%s.identity\n' "$STATE_DIR" "$(state_key "$1")"
}

is_dns_sd_running() {
    local pid_file="$1"
    local pid command

    [ -s "$pid_file" ] || return 1
    pid="$(/bin/cat "$pid_file")"

    case "$pid" in
        ''|*[!0-9]*) return 1 ;;
    esac

    /bin/kill -0 "$pid" 2>/dev/null || return 1
    command="$(/bin/ps -p "$pid" -o comm= 2>/dev/null)"
    printf '%s\n' "$command" | /usr/bin/grep -q 'dns-sd'
}

stop_airprint() {
    local pid_file="$1"
    local identity_file="$2"
    local pid

    if [ -s "$pid_file" ]; then
        pid="$(/bin/cat "$pid_file")"
        if is_dns_sd_running "$pid_file"; then
            /bin/kill "$pid" 2>/dev/null || true
        fi
    fi

    /bin/rm -f "$pid_file" "$identity_file"
}

start_airprint() {
    local printer="$1"
    local device_uri="$2"
    local ty note service_name pid_file identity_file

    ty="$(get_printer_value "$printer" 'Info')"
    note="$(get_printer_value "$printer" 'Location')"
    [ -n "$ty" ] || ty='HP LaserJet 1018'

    # The queue suffix keeps advertisements distinct for identical printers.
    service_name="$ty AirPrint ($printer)"
    pid_file="$(pid_file_for_queue "$printer")"
    identity_file="$(identity_file_for_queue "$printer")"

    /usr/bin/dns-sd -R \
      "$service_name" \
      _ipp._tcp,_universal \
      local \
      631 \
      'txtvers=1' \
      'qtotal=1' \
      "rp=printers/$printer" \
      "ty=$ty" \
      "product=($ty)" \
      "note=$note" \
      'pdl=application/pdf,image/urf' \
      'URF=V1.4,W8,CP1,RS600,DM1' \
      'Color=F' \
      'Duplex=F' &

    printf '%s\n' "$!" > "$pid_file"
    printf '%s\t%s\n' "$printer" "$device_uri" > "$identity_file"
}

ensure_airprint() {
    local printer="$1"
    local device_uri="$2"
    local pid_file identity_file

    pid_file="$(pid_file_for_queue "$printer")"
    identity_file="$(identity_file_for_queue "$printer")"

    if ! is_dns_sd_running "$pid_file"; then
        stop_airprint "$pid_file" "$identity_file"
        start_airprint "$printer" "$device_uri"
    fi
}

cleanup_stale_airprint() {
    local identity_file key pid_file printer device_uri

    for identity_file in "$STATE_DIR"/airprint-*.identity; do
        [ -f "$identity_file" ] || continue

        IFS=$'\t' read -r printer device_uri < "$identity_file"
        key="${identity_file##*/airprint-}"
        key="${key%.identity}"
        pid_file="$STATE_DIR/airprint-$key.pid"

        if ! queue_is_configured_for_uri "$printer" "$device_uri" || \
           ! is_shared "$printer" || \
           ! is_printer_available "$device_uri"
        then
            stop_airprint "$pid_file" "$identity_file"
        fi
    done
}

stop_all_airprint() {
    local identity_file key pid_file

    for identity_file in "$STATE_DIR"/airprint-*.identity; do
        [ -f "$identity_file" ] || continue

        key="${identity_file##*/airprint-}"
        key="${key%.identity}"
        pid_file="$STATE_DIR/airprint-$key.pid"
        stop_airprint "$pid_file" "$identity_file"
    done
}

trap stop_all_airprint EXIT TERM INT

ensure_state_dir

while true; do
    cleanup_stale_airprint

    while IFS=$'\t' read -r printer device_uri; do
        [ -n "$printer" ] && [ -n "$device_uri" ] || continue

        if is_shared "$printer" && is_printer_available "$device_uri"; then
            ensure_airprint "$printer" "$device_uri"
        fi
    done < <(list_hp1018_queues)

    sleep 10
done
