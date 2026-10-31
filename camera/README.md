# Signing on prudynt cameras (snapsign)

Thingino builds that run raptor sign `/snap.jpg` themselves. Builds that run
prudynt (for example the Ciao release) don't, so this folder adds signing as
a small add-on, without reflashing:

- `snapsign.c`: reads a JPEG on stdin, writes the signed JPEG to stdout. It
  compiles raptor-common's `rss_jpeg.c` unchanged, so the EXIF timestamp and
  signature format are byte-identical to raptor's, and it uses raptor's key
  file (`/etc/raptor/sign_ed25519.key`). A camera that later moves to raptor
  keeps the same key and stays registered.
- `snap-signed.jpg`: a web UI CGI that returns a fresh signed snapshot, behind
  the normal web UI login.
- `build.sh`: builds a static MIPS binary with zig.

## Build

```bash
pip install ziglang            # or install zig some other way
ZIG="python3 -m ziglang" sh camera/build.sh
```

## Install on a camera

```bash
CAM=192.168.1.185
scp -O camera/snapsign root@$CAM:/usr/bin/snapsign
scp -O camera/snap-signed.jpg root@$CAM:/var/www/x/snap-signed.jpg
ssh root@$CAM 'chmod 755 /usr/bin/snapsign /var/www/x/snap-signed.jpg && snapsign -p'
```

`snapsign -p` creates the key on first run and prints the public key to add
to `keys/devices.json`. Both files land on the camera's writable overlay;
remove them to uninstall. Reflashing with a full wipe also removes the key.

## Use

- Browser: `http://<camera>/x/snap-signed.jpg` (log in to the web UI first).
- Shell: `ssh root@<camera> 'prudyntctl snapshot -c 0 | snapsign' > photo.jpg`

Then `python3 verify.py photo.jpg`.

The capture time in the signature comes from the camera clock, so keep NTP
working.
