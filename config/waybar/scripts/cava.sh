#!/bin/bash
PIPE="$HOME/.cache/cava.fifo"
mkdir -p "$HOME/.cache"

if [[ ! -p "$PIPE" ]]; then
  rm -f "$PIPE"
  mkfifo "$PIPE"
fi

pgrep -f "cava -p.*waybar.conf" >/dev/null || cava -p "$HOME/.config/cava/waybar.conf" >/dev/null 2>&1 &

blocks=(" " "▂" "▃" "▄" "▅" "▆" "▇" "█")

# Open FIFO with write descriptor open as well to prevent EOF spin when cava isn't writing
exec 3<> "$PIPE"

while IFS= read -r -u 3 raw; do
  cleaned="${raw//;/ }"
  bars=""

  for n in $cleaned; do
    if [[ "$n" =~ ^[0-9]+$ ]]; then
      level=$(( n / 250 ))
      (( level > 7 )) && level=7
      bars+="${blocks[$level]}"
    fi
  done

  if [ -n "$bars" ]; then
    printf '{"text": "%s"}\n' "$bars"
  fi
done
