#!/bin/sh
# HELP: Native Steam Link client - stream your Steam games from the PC
# ICON: steamlink
# GRID: Steam Link

. /opt/muos/script/var/func.sh

APP_BIN="steamlink"
if command -v SETUP_APP >/dev/null 2>&1; then
	SETUP_APP "$APP_BIN" ""
else
	echo app >/tmp/act_go
	command -v SETUP_SDL_ENVIRONMENT >/dev/null 2>&1 && SETUP_SDL_ENVIRONMENT
	command -v SET_VAR >/dev/null 2>&1 && SET_VAR "system" "foreground_process" "$APP_BIN"
fi

# -----------------------------------------------------------------------------

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APP_DIR" || exit 1
chmod +x "./$APP_BIN" 2>/dev/null

# Software H.264 decode wants the big cores awake; restore the governor afterwards.
GOV_FILE=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
OLD_GOV="$(cat "$GOV_FILE" 2>/dev/null)"
[ -w "$GOV_FILE" ] && echo performance >"$GOV_FILE"

# Wi-Fi power saving adds tens of ms of jitter to every packet.
command -v iw >/dev/null 2>&1 && iw dev wlan0 set power_save off 2>/dev/null

# Name shown in Steam's list of paired devices.
SL_DEVICE_NAME="$(GET_VAR "device" "board/name" 2>/dev/null)"
export SL_DEVICE_NAME="muOS ${SL_DEVICE_NAME:-handheld}"

CAFFEINE="$(command -v CAFFEINE 2>/dev/null || true)"
[ -n "$CAFFEINE" ] && "$CAFFEINE" on

"./$APP_BIN" >"$APP_DIR/steamlink.stderr.log" 2>&1

[ -n "$CAFFEINE" ] && "$CAFFEINE" off
[ -n "$OLD_GOV" ] && [ -w "$GOV_FILE" ] && echo "$OLD_GOV" >"$GOV_FILE"
