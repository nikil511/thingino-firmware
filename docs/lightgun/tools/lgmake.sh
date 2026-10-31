#!/bin/sh
# run make inside the thingino builder container with output kept in the VM volume
# usage: /Volumes/thingino/lgmake.sh [make targets...]
cd /Volumes/thingino/thingino-firmware || exit 1
exec podman run --rm --userns=keep-id -i --volumes-from thingino-dl-cache \
	-v "$PWD":/workspace -v thingino-output:/workspace/output:U -v "$PWD/overrides":/overrides \
	-w /workspace -e TERM=xterm-256color -e BR2_DL_DIR=/dl \
	ghcr.io/themactep/thingino-builder-image:latest \
	bash -c "sudo update-alternatives --install /usr/bin/install install /usr/bin/gnuinstall 100 2>/dev/null; make CAMERA=${CAMERA:-sonoff_s2_t23n_sc2336_atbm6012bx} $*"
