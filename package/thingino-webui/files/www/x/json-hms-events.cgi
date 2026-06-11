#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

EVENTS_FILE="/tmp/hms/events.json"

# POST with action=clear to clear events
if [ "$REQUEST_METHOD" = "POST" ]; then
    read -r body
    action=$(printf '%s' "$body" | sed -n 's/.*action=\([^&]*\).*/\1/p')
    if [ "$action" = "clear" ]; then
        rm -f "$EVENTS_FILE"
        printf '[]'
        exit 0
    fi
    printf '{"error":"unknown action"}'
    exit 0
fi

if [ -f "$EVENTS_FILE" ] && [ -s "$EVENTS_FILE" ]; then
    printf '[' > /tmp/hms_events_out.$$
    first=1
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        [ "$first" = "0" ] && printf ',' >> /tmp/hms_events_out.$$
        first=0
        printf '%s' "$line" >> /tmp/hms_events_out.$$
    done < "$EVENTS_FILE"
    printf ']' >> /tmp/hms_events_out.$$
    cat /tmp/hms_events_out.$$
    rm -f /tmp/hms_events_out.$$
else
    printf '[]'
fi
