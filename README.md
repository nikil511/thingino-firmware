# Camera photo origin: device keys and verifier

This branch is a public registry of camera public keys, plus reference code
that lets anyone check whether a JPEG was taken and signed by one of those
cameras. It is a **demo** of the concept. Read [What this proves](#what-this-proves)
before relying on it.

- `keys/devices.json`: the registered cameras and their Ed25519 public keys.
- `verify.py`: command-line verifier. Python 3 standard library only.
- `index.html`: the same check in the browser. The photo never leaves the page.
- `AGENTS.md`: step-by-step instructions for an AI agent asked to verify a photo.
- `VALIDATION.md`: how validation works, byte by byte, with a worked example.
- `tests/`: sample photos signed by the real camera code with a public test key.
- `camera/`: `snapsign`, the signing add-on for cameras that run prudynt.

This branch has no shared history with the firmware branches (`git checkout --orphan`).

## Verify a photo

You need the **original file**, byte for byte as the camera produced it.
Messaging apps, screenshots, photo editors, "save as" and resizing all rewrite
the file and remove the signature.

```bash
python3 verify.py photo.jpg
```

From a clone of this branch, `verify.py` reads `keys/devices.json` next to it.
Copied anywhere else, it downloads the registry from `REGISTRY_URL` at the top
of the script. Other options:

```bash
python3 verify.py --json photo.jpg                  # machine-readable result
python3 verify.py --key <64-hex-public-key> photo.jpg   # check one specific key
python3 verify.py --registry other/devices.json photo.jpg
```

| Result | Exit | Meaning |
| --- | --- | --- |
| `VERIFIED` | 0 | Signed by a registered camera, and not changed since. |
| `INVALID` | 1 | Has a camera signature, but the bytes changed after signing (or the signature was moved). |
| `NOT_SIGNED` | 3 | No camera signature. Never signed, or re-encoded on the way. |
| `UNKNOWN_DEVICE` | 4 | Validly formatted signature from a key not in the registry. |
| error | 2 | Bad arguments, unreadable file or registry. |

Or open `index.html` (served over HTTP, e.g. GitHub Pages for this branch) and
drop the photo on it. It needs a browser with WebCrypto Ed25519: Chrome 137+,
Firefox 129+ or Safari 17+.

Run the self-test with `sh tests/run.sh`.

## How the camera signs

Thingino's raptor streamer signs every `/snap.jpg` it serves
(`[http] sign_snapshots`). On first use the camera generates an Ed25519 key
pair from `/dev/urandom` and stores the 32-byte seed at
`/etc/raptor/sign_ed25519.key`, next to a hex copy of the public key at
`/etc/raptor/sign_ed25519.key.pub`.

Each snapshot gets an APP15 segment inserted right before the final `FF D9`:

| Bytes | Content |
| --- | --- |
| 2 | `FF EF` (APP15 marker) |
| 2 | `00 5B` (segment length 91, excluding the marker) |
| 16 | `b050bf07-8856-4598-869e-44c4dc208f26` (raptor signing UUID) |
| 1 | `01` (format version) |
| 8 | key fingerprint: first 8 bytes of SHA-512(public key) |
| 64 | Ed25519 signature |

The signed message is every byte of the file before the `FF EF` marker. With
`[http] exif_timestamp` on, that includes an EXIF block right after `FF D8`
holding the capture time in UTC (microseconds), so the timestamp is signed too.
It is only as accurate as the camera's clock (NTP).

Raptor's own `rverify` tool checks the same format:
`rverify -k <pubkey> photo.jpg`.

## Register a camera

Cameras running prudynt instead of raptor (for example the Ciao release) need
the `snapsign` add-on first. It signs in the same format with the same key
file; see [camera/README.md](camera/README.md). The steps below are for raptor.

1. On the camera, make sure signing is on in `/etc/raptor.conf`:

   ```ini
   [http]
   sign_snapshots = true
   exif_timestamp = true
   ```

   then restart the streamer (`/etc/init.d/S31raptor restart`) or reboot.

2. Take one snapshot so the key exists, then read the public key:

   ```bash
   curl -sk -o first.jpg "https://<camera-ip>:<http-port>/snap.jpg"
   ssh root@<camera-ip> cat /etc/raptor/sign_ed25519.key.pub
   ```

   The port is under `[http]` in `/etc/raptor.conf`.

3. Add an entry to `keys/devices.json` and commit it:

   ```json
   {
     "id": "wxm-demo-cam-01",
     "description": "Demo camera on the office roof",
     "public_key": "<the 64 hex characters>",
     "added": "2026-10-31"
   }
   ```

4. Check it end to end: `python3 verify.py first.jpg` should print `VERIFIED`.

Never publish `/etc/raptor/sign_ed25519.key` itself. Deleting it (or
reflashing the camera) makes the camera generate a new key; register that one
and keep the old entry so older photos still verify.

## What this proves

A `VERIFIED` result means: these exact bytes were signed by whoever holds the
private key that matches a registered public key, and nothing changed since.

It does **not** prove the scene was real, and it does not stop the camera's
owner. In this demo the private key sits in the camera's flash, so anyone with
root access, the serial console or a flash programmer can copy it and sign any
image they like. Pointing the camera at a screen also defeats it. A production
version would keep the key in a secure element, lock down the firmware, and
add server-side plausibility checks.
