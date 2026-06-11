#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

HMS_DIR="/tmp/hms"

if [ "$REQUEST_METHOD" = "POST" ]; then
  read -r body
  cmd=$(echo "$body" | sed -n 's/.*cmd=\([^&]*\).*/\1/p')
  case "$cmd" in
    arm_home) echo "home" > "$HMS_DIR/arm_cmd"; printf '{"status":"ok","cmd":"arm_home"}' ;;
    arm_away) echo "away" > "$HMS_DIR/arm_cmd"; printf '{"status":"ok","cmd":"arm_away"}' ;;
    disarm)
      pin=$(echo "$body" | sed -n 's/.*pin=\([^&]*\).*/\1/p')
      echo "disarm:$pin" > "$HMS_DIR/arm_cmd"
      printf '{"status":"ok","cmd":"disarm"}'
      ;;
    panic)
      echo "panic" > "$HMS_DIR/arm_cmd"
      printf '{"status":"ok","cmd":"panic"}'
      ;;
    *) printf '{"error":"unknown cmd"}';;
  esac
  exit 0
fi

printf '{"status":"ready"}'
