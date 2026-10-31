#!/bin/sh
# Check verify.py against sample photos signed by raptor's own code
# (rss_jpeg_sign + monocypher) with a publicly known test key.
cd "$(dirname "$0")/.." || exit 2

fail=0
check() {
	python3 -I verify.py --registry tests/test-keys.json "tests/$1" >/dev/null
	got=$?
	if [ "$got" -eq "$2" ]; then
		echo "ok   $1 -> exit $got"
	else
		echo "FAIL $1 -> exit $got, expected $2"
		fail=1
	fi
}

check sample-signed.jpg 0
check sample-tampered.jpg 1
check sample-appended.jpg 1
check sample-resaved.jpg 3

exit $fail
