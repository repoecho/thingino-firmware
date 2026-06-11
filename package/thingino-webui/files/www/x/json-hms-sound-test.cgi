#!/bin/sh
. /var/www/x/auth.sh
require_auth

printf 'Content-Type: application/json\r\n'
printf 'Cache-Control: no-cache\r\n'
printf 'Connection: close\r\n'
printf '\r\n'

if [ "$REQUEST_METHOD" = "POST" ]; then
  read -r body
  track=$(echo "$body" | sed -n 's/.*track=\([^&]*\).*/\1/p')
  file="/usr/share/sounds/${track}.opus"
  if [ -f "$file" ]; then
    rac play "$file" 2>/dev/null
    rc=$?
    [ "$rc" = 0 ] && printf '{"status":"playing","track":"%s"}' "$track" || printf '{"status":"error","message":"rac failed"}'
  else
    printf '{"status":"error","message":"track not found"}'
  fi
  exit 0
fi

# List available tracks
printf '{"tracks":['
first=1
for f in /usr/share/sounds/*.opus; do
  [ -f "$f" ] || continue
  b=$(basename "$f" .opus)
  [ "$first" = "0" ] && printf ','
  first=0
  printf '"%s"' "$b"
done
printf ']}'
