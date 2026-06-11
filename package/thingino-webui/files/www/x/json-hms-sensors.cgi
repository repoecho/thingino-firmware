#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

HMS_DIR="/tmp/hms"
CONFIG_DIR="/etc/hms"
SENSORS_DIR="$HMS_DIR/sensors"
CONFIG_FILE="$CONFIG_DIR/config.json"
GW_STATUS_FILE="$HMS_DIR/gateway_status"

gw=$(cat "$GW_STATUS_FILE" 2>/dev/null || echo "offline")
printf '{"gateway_status":"%s","sensors":[' "$gw"

first=1
for sd in "$SENSORS_DIR"/*/; do
  [ -d "$sd" ] || continue
  mac=$(basename "$sd")
  state=$(cat "$sd/state" 2>/dev/null || echo "unknown")
  sclass=$(cat "$sd/class" 2>/dev/null || echo "?")
  battery=$(cat "$sd/battery" 2>/dev/null || echo "0")
  rssi_val=$(cat "$sd/rssi" 2>/dev/null || echo "0")

  # Get config from sensors.json or config.json
  name="$mac"
  stype="entry"
  config_name=$(jct "$CONFIG_FILE" get "sensors.$mac.name" 2>/dev/null | tr -d '"')
  [ -n "$config_name" ] && name="$config_name"
  config_type=$(jct "$CONFIG_FILE" get "sensors.$mac.type" 2>/dev/null | tr -d '"')
  [ -n "$config_type" ] && stype="$config_type"

  [ "$first" = "0" ] && printf ','
  first=0
  printf '{"mac":"%s","name":"%s","class":"%s","state":"%s","battery":%s,"rssi":%s,"type":"%s"}' \
    "$mac" "$name" "$sclass" "$state" "$battery" "$rssi_val" "$stype"
done

printf ']}'
