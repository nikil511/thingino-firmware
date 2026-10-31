#!/bin/sh
# Expects lgcap and the patched module (as sc2336_mode.ko) in /mnt/mmcblk0p1/lightgun/bin on the camera.
# lgmodes MODE "FPS..." : load sensor_mode, smoke-test each fps natively (fallback: aligned scaler size)
D=/mnt/mmcblk0p1/lightgun; m=$1; R=$D/results/mode$m; mkdir -p $R
rmmod sensor_sc2336_t23 2>/dev/null; insmod $D/bin/sc2336_mode.ko sensor_mode=$m || { echo "insmod failed"; exit 1; }
dmesg | grep -E "sc2336 (mode|window)" | tail -2
W=$(cat /proc/jz/sensor/width); H=$(cat /proc/jz/sensor/height)
for f in $2; do
	o=""; [ $((H % 8)) -ne 0 ] && o="-o ${W}x$((H / 8 * 8))"
	echo "--- mode $m ${W}x${H} @ $f $o"
	$D/bin/lgcap -n 600 -W 60 -f $f $o -s $R/frame_f$f > $R/f$f.txt 2>&1
	grep -E "^\[E\]|SetSensorFPS|^sensor fps|^frame:|^delivered|^isp fps|^median|^dropped|^processed|^interval|^latency" $R/f$f.txt
done
dmesg | tail -3 | grep -viE "wait stop|streamoff"
