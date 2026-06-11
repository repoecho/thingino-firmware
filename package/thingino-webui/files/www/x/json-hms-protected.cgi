#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

CONFIG_FILE="/etc/hms/config.json"

total=0
week=0
now=$(date +%s)
week_ago=$((now - 604800))

if [ -f "$CONFIG_FILE" ]; then
  data=$(jct "$CONFIG_FILE" get protected_hours 2>/dev/null | tr -d '{}"')
  # Parse: key:value,key:value
  OIFS="$IFS"; IFS=','
  for pair in $data; do
    IFS=:; set -- $pair; d="$1"; h="$2"
    IFS="$OIFS"
    [ -z "$h" ] && continue
    total=$((total + h))
    ed=$(date -d "$d" +%s 2>/dev/null)
    if [ -n "$ed" ] && [ "$ed" -ge "$week_ago" ] 2>/dev/null; then
      week=$((week + h))
    fi
  done
  IFS="$OIFS"
fi

echo "{\"total_hours\":$total,\"week_hours\":$week}"
