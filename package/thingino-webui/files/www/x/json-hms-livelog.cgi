#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

LIVE_LOG="/tmp/hms/live.log"

if [ -f "$LIVE_LOG" ] && [ -s "$LIVE_LOG" ]; then
    printf '['
    first=1
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        [ "$first" = "0" ] && printf ','
        first=0
        printf '%s' "$line"
    done < "$LIVE_LOG"
    printf ']'
else
    printf '[]'
fi
