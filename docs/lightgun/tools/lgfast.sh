#!/bin/sh
# Expects lgcap and the patched module (as sc2336_mode.ko) in /mnt/mmcblk0p1/lightgun/bin on the camera.
# blanking threshold probes + workload/stability for the fast modes
D=/mnt/mmcblk0p1/lightgun; B=$D/bin
mode() { rmmod sensor_sc2336_t23 2>/dev/null; insmod $B/sc2336_mode.ko sensor_mode=$1; }
sum() { grep -E "^\[E\]|^delivered|^median|^dropped|^processed|^latency|^proc_us|^cpu" "$1" | sed -E 's/ \(frames returned.*//; s/latency_us +mean +[0-9.]+ +min +[0-9]+ +/lat /; s/proc_us +mean +[0-9.]+ +min +[0-9]+ +/proc /; s/ us \(.*//'; }
run() { m=$1; n=$2; shift 2; R=$D/results/mode$m; mkdir -p $R; echo "--- mode $m $n: lgcap $*"; $B/lgcap "$@" > $R/$n.txt 2>&1; sum $R/$n.txt; }
mode 4; run 4 probe_f88 -n 600 -f 88
mode 5; run 5 probe_f115 -n 600 -f 115
        run 5 f110_320x184_work -n 3300 -f 110 -o 320x184 -w 1
        run 5 f110_native_work -n 3300 -f 110 -w 1
        run 5 long_f110_320x184_work -n 10000 -f 110 -o 320x184 -w 1
mode 6; run 6 probe_f92 -n 600 -f 92
        run 6 f90_640x120_work -n 2700 -f 90 -o 640x120 -w 1
mode 2; run 2 f60_640x184_work -n 1800 -f 60 -o 640x184 -w 1
        run 2 long_f60_640x184_work -n 10000 -f 60 -o 640x184 -w 1
