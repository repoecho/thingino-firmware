#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

CONFIG_FILE="/etc/hms/config.json"
mkdir -p /etc/hms

if [ ! -f "$CONFIG_FILE" ]; then
  echo '{"system_pin":"1234","entry_delay_home":30,"entry_delay_away":15,"exit_delay_home":45,"exit_delay_away":60,"silent_entry":false,"silent_exit":false,"tune_enabled":true,"tune_volume":"mid","tune_sensors":[],"sensors":{},"protected_hours":{}}' > "$CONFIG_FILE"
fi

if [ "$REQUEST_METHOD" = "POST" ]; then
  read -r body
  action=$(echo "$body" | sed -n 's/.*action=\([^&]*\).*/\1/p')
  case "$action" in
    import)
      data=$(echo "$body" | sed -n 's/.*data=\([^&]*\).*/\1/p' | sed 's/+/ /g; s/%/\\x/g' | xargs -0 printf '%b' 2>/dev/null)
      echo "$data" | jct - validate 2>/dev/null
      if [ $? -ne 0 ]; then
        echo "$data" | jq -e '.system_pin' >/dev/null 2>&1 || { printf '{"error":"invalid config"}'; exit 0; }
      fi
      echo "$data" > "$CONFIG_FILE"
      printf '{"status":"imported"}'
      ;;
    update_sensor)
      mac=$(echo "$body" | sed -n 's/.*mac=\([^&]*\).*/\1/p')
      name=$(echo "$body" | sed -n 's/.*name=\([^&]*\).*/\1/p' | sed 's/+/ /g; s/%20/ /g')
      stype=$(echo "$body" | sed -n 's/.*sensor_type=\([^&]*\).*/\1/p')
      tune=$(echo "$body" | sed -n 's/.*tune=\([^&]*\).*/\1/p')
      jct "$CONFIG_FILE" set "sensors.$mac.name" "$name" >/dev/null 2>&1
      jct "$CONFIG_FILE" set "sensors.$mac.type" "$stype" >/dev/null 2>&1
      jct "$CONFIG_FILE" set "sensors.$mac.tune" "$tune" >/dev/null 2>&1
      printf '{"status":"ok","mac":"%s"}' "$mac"
      ;;
    *)
      # Field update: field=value
      field=$(echo "$body" | cut -d= -f1)
      value=$(echo "$body" | cut -d= -f2-)
      case "$field" in
        system_pin) jct "$CONFIG_FILE" set "$field" "$value" >/dev/null 2>&1 ;;
        entry_delay_home|entry_delay_away|exit_delay_home|exit_delay_away)
          jct "$CONFIG_FILE" set "$field" "$(echo "$value" | tr -cd 0-9)" >/dev/null 2>&1 ;;
        silent_entry|silent_exit|tune_enabled)
          jct "$CONFIG_FILE" set "$field" "$value" >/dev/null 2>&1 ;;
        tune_volume) jct "$CONFIG_FILE" set "$field" "$value" >/dev/null 2>&1 ;;
        tune_sensors) jct "$CONFIG_FILE" set "$field" "$value" >/dev/null 2>&1 ;;
        *) printf '{"error":"unknown field"}'; exit 0 ;;
      esac
      printf '{"status":"updated","field":"%s"}' "$field"
      ;;
  esac
  exit 0
fi

cat "$CONFIG_FILE"
