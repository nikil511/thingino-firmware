#!/bin/sh
# lgbench - run the standard lgcap matrix for the sensor mode currently loaded.
#
#   lgbench [OUTDIR]          default OUTDIR: first mounted SD card, /lightgun
#
# Env: N (frames per run, 1800), LONG (frames for the stability run, 10000),
#      FPS_LIST (sensor fps to test, default: max_fps), OUTS (scaled sizes),
#      EXPO (manual exposure lines, default auto), LED (GPIO for the LED test).
# The streamer must not hold the ISP, so it is stopped first (restart: reboot,
# or /etc/init.d/S31prudynt start).

set -u

sd=$(awk '$2 ~ /^\/mnt\// {print $2; exit}' /proc/mounts)
OUT=${1:-${sd:-/root}/lightgun}
N=${N:-1800}
LONG=${LONG:-10000}

W=$(cat /proc/jz/sensor/width)
H=$(cat /proc/jz/sensor/height)
MAX=$(cat /proc/jz/sensor/max_fps)
FPS_LIST=${FPS_LIST:-$MAX}
MODE=${W}x${H}
DIR=$OUT/$MODE
mkdir -p "$DIR" || exit 1

# ISP scaler targets, near the sensor aspect.  libimp (T23 1.3.0) rejects
# channel sizes whose width isn't a multiple of 16 or height a multiple of 8
# (check_framesource_chn_attr), so 540 -> 544 and 180 -> 184.
if [ -z "${OUTS:-}" ]; then
	if [ $((W * 3)) -eq $((H * 4)) ]; then
		OUTS="640x480 320x240 160x120"
	elif [ $((W * 9)) -eq $((H * 16)) ]; then
		OUTS="960x544 640x360 320x184"
	else
		OUTS="$((W / 2 / 16 * 16))x$((H / 2 / 8 * 8)) $((W / 3 / 16 * 16))x$((H / 3 / 8 * 8)) $((W / 6 / 16 * 16))x$((H / 6 / 8 * 8))"
	fi
fi
EX=""
[ -n "${EXPO:-}" ] && EX="-e $EXPO"

for s in /etc/init.d/S32prudyntwd /etc/init.d/S31prudynt; do
	[ -x "$s" ] && "$s" stop >/dev/null 2>&1
done
killall prudynt raptor 2>/dev/null
sleep 2

run() { # name, lgcap args...
	name=$1
	shift
	echo "=== $MODE $name: lgcap $*" | tee "$DIR/$name.txt"
	lgcap "$@" >>"$DIR/$name.txt" 2>&1
	grep -E "^(delivered|isp fps|dropped|processed|latency|proc_us|cpu|sensor fps)" "$DIR/$name.txt"
}

{
	echo "mode $MODE max_fps $MAX date $(date)"
	uname -a
	cat /proc/cmdline
	grep -E "MemTotal|MemAvailable" /proc/meminfo
	cat /sys/module/sensor_sc2336_t23/parameters/* 2>/dev/null
} >"$DIR/env.txt"

for f in $FPS_LIST; do
	run "native_f${f}" -n "$N" -f "$f" $EX -s "$DIR/fov_native_f${f}"
	run "native_f${f}_work" -n "$N" -f "$f" -w 1 $EX
	for o in $OUTS; do
		run "scale_${o}_f${f}" -n "$N" -f "$f" -o "$o" $EX -s "$DIR/fov_${o}_f${f}"
		run "scale_${o}_f${f}_work" -n "$N" -f "$f" -o "$o" -w 1 $EX
	done
	# separate scaler channel next to the full-size channel
	o=$(echo $OUTS | awk '{print $2}')
	run "ch1_${o}_f${f}_work" -n "$N" -f "$f" -c "$o" -r 1 -w 1 $EX
done

o=$(echo $OUTS | awk '{print $2}')
run "long_${o}_work" -n "$LONG" -f "$MAX" -o "$o" -w 1 $EX -C "$DIR/long_${o}.csv"
[ -n "${LED:-}" ] && run "led_${o}" -n 600 -f "$MAX" -o "$o" -L "$LED" $EX

echo "results in $DIR"
