# How photo signature validation works

This document explains, byte by byte, how to decide whether a JPEG was signed
by a camera registered on this branch. `verify.py` implements exactly these
steps; the procedure for an agent is in `AGENTS.md`.

## The idea

Each camera holds an Ed25519 key pair. The **private key** never leaves the
camera (`/etc/raptor/sign_ed25519.key`). The **public key** is published in
`keys/devices.json` on this branch.

When the camera produces a snapshot it:

1. inserts an EXIF block with the capture time right after the JPEG start
   marker,
2. signs every byte of the resulting file with its private key,
3. appends the 64-byte signature in a small APP15 segment just before the
   JPEG end marker.

Anyone with the public key can recompute the check. If even one bit of the
signed bytes changes, the check fails. Image viewers ignore the APP15 segment,
so the photo still opens normally.

## What you are trusting

- **The registry.** A `VERIFIED` result means "signed by a key listed in
  `keys/devices.json`". Whoever can push to this branch decides which keys
  are listed. In a full clone (not `--depth 1`), `git log -p keys/devices.json`
  shows when each key was added and by whom.
- **The camera kept its key secret.** In this demo the key is a file in the
  camera's flash. Someone with root access to that camera could copy it.
- **The camera clock.** The capture time is signed, so nobody can change it
  afterwards, but it is only as right as the camera's clock (NTP) was.

A signature does not show the scene was real: a photo of a screen, taken by
the camera, verifies.

## File layout

```
offset 0      FF D8                 SOI, start of image
offset 2      FF E1 00 6E 'Exif'..  EXIF APP1, 112 bytes, holds the capture time
offset 114    ...                   the JPEG as the camera's encoder produced it
len - 95      FF EF ...             APP15 signature segment, 93 bytes
len - 2       FF D9                 EOI, end of image
```

The EXIF block is present on every photo from `snapsign`, and on raptor
photos when `[http] exif_timestamp` is on. It is part of the signed bytes.

The signature segment, 93 bytes, starting at `len - 95`:

| Offset in segment | Size | Value |
| --- | --- | --- |
| 0 | 2 | `FF EF`, APP15 marker |
| 2 | 2 | `00 5B`, segment length 91 (big-endian, excludes the marker) |
| 4 | 16 | `b0 50 bf 07 88 56 45 98 86 9e 44 c4 dc 20 8f 26`, raptor signing UUID |
| 20 | 1 | `01`, format version |
| 21 | 8 | key fingerprint: first 8 bytes of SHA-512(public key) |
| 29 | 64 | Ed25519 signature (R then S, as in RFC 8032) |

**Signed message:** bytes `[0, len - 95)` of the file, i.e. everything before
the `FF EF` marker. The signature segment and the final `FF D9` are not
signed.

## Validation steps

Each step either continues or ends with the result shown.

1. **Read the file as raw bytes.** Never decode and re-encode the image.
2. **Is it a JPEG?** The first two bytes must be `FF D8`.
   Otherwise: `NOT_SIGNED`.
3. **Find the segment at the end.** The last two bytes must be `FF D9`, and
   the 20 bytes at `len - 95` must be `FF EF 00 5B` followed by the UUID.
   - If not, but `FF EF 00 5B` + UUID appears somewhere else in the file,
     the segment was there once and bytes were added or removed afterwards:
     `INVALID`.
   - If it appears nowhere: `NOT_SIGNED`.
4. **Check the version.** Segment byte 20 must be `01`. Otherwise `INVALID`.
5. **Find the key.** Read the 8-byte fingerprint from segment bytes 21-28.
   For each device in `keys/devices.json`, compute
   `SHA-512(bytes.fromhex(public_key))[:8]` and compare.
   No match: `UNKNOWN_DEVICE`. The fingerprint is only a lookup hint;
   the next step is what proves anything.
6. **Verify the signature.** Run standard Ed25519 verification (RFC 8032)
   with that public key, the 64-byte signature from segment bytes 29-92,
   and the message `file[0 : len - 95]`.
   Valid: `VERIFIED`. Invalid: `INVALID`. Because the fingerprint already
   matched a registered key, an invalid signature here means the file
   changed after signing, not that a different camera signed it.
7. **Read the capture time** (optional). If bytes 2-3 are `FF E1` and bytes
   6-11 are `Exif\0\0`, the TIFF block starts at byte 12. The date is the
   19 ASCII characters at TIFF offset 68 (`YYYY:MM:DD HH:MM:SS`, UTC) and the
   microseconds are the 6 ASCII digits at TIFF offset 95. Report it as signed
   only when step 6 succeeded.

## Worked example

`tests/sample-signed.jpg` is 11904 bytes, signed with the public test key in
`tests/test-keys.json`. The segment starts at `11904 - 95 = 11809` (`0x2e21`):

```
00002e21  ff ef 00 5b b0 50 bf 07 88 56 45 98 86 9e 44 c4   marker, length, UUID...
00002e31  dc 20 8f 26 01 ed 42 42 ea d4 ac 69 48 02 b9 57   ...UUID, version 01, fingerprint ed4242ead4ac6948, signature...
00002e41  5f 55 16 03 c4 be e6 75 c6 f8 dd 88 7a 6e b9 71
00002e51  6c 9c 5e c0 16 a9 e4 ef 05 ab fe 6e 65 39 56 a7
00002e61  74 52 56 08 fd d3 d8 84 2a 7a 50 b4 1c af 34 75
00002e71  5f ca 62 c5 38 f0 4d bb f6 35 d3 0c 08 ff d9      ...signature, EOI
```

- Fingerprint `ed4242ead4ac6948` = first 8 bytes of SHA-512 of public key
  `03a107bf...125531b8`, so that is the key to use.
- The message is the first 11809 bytes.
- The EXIF date at file offset 12 + 68 reads `2026:10:31 16:00:00`, and the
  microseconds at 12 + 95 read `123456`, so the capture time is
  `2026-10-31T16:00:00.123456Z`.

## Independent cross-check

`verify.py` contains its own small Ed25519 implementation so it needs no
packages. To check its answer with a well-known library instead
(`pip install cryptography`):

```python
import hashlib, json, sys
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

photo, registry = sys.argv[1], sys.argv[2]
d = open(photo, "rb").read()
seg = d[-95:]
assert d[:2] == b"\xff\xd8" and d[-2:] == b"\xff\xd9", "not a complete JPEG"
assert seg[:4] == b"\xff\xef\x00\x5b", "no signature segment at the end"
assert seg[4:20].hex() == "b050bf0788564598869e44c4dc208f26", "wrong UUID"
assert seg[20] == 1, "unknown version"
fp, sig, msg = seg[21:29], seg[29:93], d[:-95]
for dev in json.load(open(registry))["devices"]:
    pub = bytes.fromhex(dev["public_key"])
    if hashlib.sha512(pub).digest()[:8] == fp:
        try:
            Ed25519PublicKey.from_public_bytes(pub).verify(sig, msg)
            print("VERIFIED", dev["id"])
        except InvalidSignature:
            print("INVALID", dev["id"])
        break
else:
    print("UNKNOWN_DEVICE", fp.hex())
```

Run it as `python3 -I crosscheck.py photo.jpg keys/devices.json`.

## What removes or breaks a signature

| What happened to the file | Typical result |
| --- | --- |
| Downloaded or copied as a file (curl, scp, USB, email attachment) | Still `VERIFIED` |
| Opened in a viewer without saving | Still `VERIFIED` |
| Re-saved, cropped, resized, filtered, or edited in any way | `NOT_SIGNED` (segment dropped) or `INVALID` |
| Sent through WhatsApp, Signal, Telegram (as photo), social media | `NOT_SIGNED`: re-encoded |
| Screenshot of the photo | `NOT_SIGNED` |
| Metadata edited or stripped (exiftool, "remove location") | `INVALID` or `NOT_SIGNED` |
| Extra bytes appended after the end | `INVALID` |
