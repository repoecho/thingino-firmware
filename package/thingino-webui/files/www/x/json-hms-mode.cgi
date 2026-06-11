#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

HMS_DIR="/tmp/hms"
STATE_FILE="$HMS_DIR/state"
ARMED_TYPE_FILE="$HMS_DIR/armed_type"
ALARM_FILE="$HMS_DIR/alarm"
CONFIG_FILE="/etc/hms/config.json"

if [ "$REQUEST_METHOD" = "POST" ]; then
  read -r body
  action=$(echo "$body" | sed -n 's/.*action=\([^&]*\).*/\1/p')
  case "$action" in
    arm)
      type=$(echo "$body" | sed -n 's/.*type=\([^&]*\).*/\1/p')
      case "$type" in home|away)
        echo "$type" > "$HMS_DIR/arm_cmd"
        printf '{"status":"arming","type":"%s"}' "$type"
        ;; *) printf '{"error":"invalid arm type"}';;
      esac ;;
    disarm|cancel_alarm)
      pin=$(echo "$body" | sed -n 's/.*pin=\([^&]*\).*/\1/p')
      expected=$(jct "$CONFIG_FILE" get system_pin 2>/dev/null | tr -d '"')
      if [ "$pin" = "$expected" ]; then
        echo 0 > "$HMS_DIR/pin_fails"
        echo "disarm:$pin" > "$HMS_DIR/arm_cmd"
        printf '{"status":"disarmed"}'
      else
        n=$(cat "$HMS_DIR/pin_fails" 2>/dev/null || echo 0)
        n=$((n + 1)); echo "$n" > "$HMS_DIR/pin_fails"
        state=$(cat "$STATE_FILE" 2>/dev/null)
        if [ "$n" -ge 3 ] && [ "$state" = "ENTRY_DELAY" ]; then
          echo "panic" > "$HMS_DIR/arm_cmd"
          printf '{"status":"alarm_triggered","message":"too many wrong attempts","attempts":%d}' "$n"
        else
          printf '{"status":"error","message":"wrong pin","attempts":%d}' "$n"
        fi
      fi ;;
    *) printf '{"error":"unknown action"}';;
  esac
  exit 0
fi

state=$(cat "$STATE_FILE" 2>/dev/null || echo "DISARMED")
armed=$(cat "$ARMED_TYPE_FILE" 2>/dev/null || echo "none")
de=$(cat "$HMS_DIR/delay_end" 2>/dev/null || echo 0)
ds=$(cat "$HMS_DIR/delay_sensor" 2>/dev/null || echo "")
af=$(cat "$HMS_DIR/pin_fails" 2>/dev/null || echo 0)

printf '{"state":"%s","armed_type":"%s","delay_end":%s,"delay_sensor":"%s","pin_fails":%s}' \
  "$state" "$armed" "$de" "$ds" "$af"
