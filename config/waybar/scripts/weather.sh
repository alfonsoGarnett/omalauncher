#!/usr/bin/env python3
import json
import os
import sys
import time
import urllib.request

CACHE_FILE = os.path.expanduser("~/.cache/waybar-weather.json")
CACHE_TIME = 900  # 15 minutes

if os.path.exists(CACHE_FILE):
    try:
        age = time.time() - os.path.getmtime(CACHE_FILE)
        if age < CACHE_TIME:
            with open(CACHE_FILE, "r", encoding="utf-8") as f:
                print(f.read().strip())
                sys.exit(0)
    except Exception:
        pass

# Condition words in English and Spanish, so a Spanish-language forecast gets its icons too.
def get_icon(condition: str) -> str:
    c = condition.lower()
    if any(k in c for k in ["thunder", "storm", "tormenta"]):
        return "󰖓"
    if any(k in c for k in ["snow", "nieve", "flurry", "blizzard"]):
        return "󰖘"
    if any(k in c for k in ["heavy rain", "lluvia fuerte"]):
        return "󰖗"
    if any(k in c for k in ["rain", "shower", "drizzle", "lluvia", "llovizna"]):
        return "󰖖"
    if any(k in c for k in ["mist", "fog", "haze", "niebla", "neblina"]):
        return "󰖑"
    if any(k in c for k in ["partly cloudy", "parcialmente nublado"]):
        return "󰖕"
    if any(k in c for k in ["cloud", "overcast", "nublado", "nuboso"]):
        return "󰖐"
    if any(k in c for k in ["clear", "sunny", "despejado", "soleado"]):
        return "󰖙"
    return "󰖐"

try:
    req = urllib.request.Request("http://wttr.in?format=%C+%t", headers={"User-Agent": "curl/8.0"})
    with urllib.request.urlopen(req, timeout=3) as resp:
        data = resp.read().decode("utf-8").strip()

    if data and "Unknown" not in data and "<html" not in data:
        # data is e.g. "Light rain shower +15°C"
        parts = data.rsplit(" ", 1)
        if len(parts) == 2:
            condition, temp = parts[0].strip(), parts[1].strip()
        else:
            condition, temp = "Weather", data

        icon = get_icon(condition)
        text = f"{icon} {temp}"
        tooltip = f"{condition}, {temp}"
        result = json.dumps({"text": text, "tooltip": tooltip})

        os.makedirs(os.path.dirname(CACHE_FILE), exist_ok=True)
        with open(CACHE_FILE, "w", encoding="utf-8") as f:
            f.write(result)
        print(result)
        sys.exit(0)
except Exception:
    pass

if os.path.exists(CACHE_FILE):
    try:
        with open(CACHE_FILE, "r", encoding="utf-8") as f:
            print(f.read().strip())
            sys.exit(0)
    except Exception:
        pass

print(json.dumps({"text": "󰖐 --°C", "tooltip": "Weather unavailable"}))
