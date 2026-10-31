# Agent instructions: verify that a photo came from a registered camera

You are here because someone has a JPEG and wants to know whether one of the
cameras registered on this branch took it. You can answer that with a local
check; nothing needs to be uploaded anywhere.

Read `VALIDATION.md` if you need to understand or explain how the check
works. The steps below are enough to run it.

## In one minute

Each registered camera signs its photos with a private key that stays on the
camera. The matching public keys are in `keys/devices.json`. The signature is
stored inside the JPEG, in a 93-byte block right before the file's last two
bytes, and covers every byte before it, including the capture time. If the
signature checks out against a registered key, the file is byte-for-byte what
that camera produced.

## 1. Get this branch

If you are not already in a checkout of it:

```bash
git clone --branch device-keys --single-branch --depth 1 \
  https://github.com/nikil511/thingino-firmware.git photo-origin
cd photo-origin
```

This branch shares no files with the firmware branches, so the clone is
small. If you cannot use git, downloading `verify.py` alone is enough; it
fetches the key list itself:

```bash
curl -sSfLO https://raw.githubusercontent.com/nikil511/thingino-firmware/device-keys/verify.py
```

## 2. Get the original photo bytes

- Use the file exactly as the person provided it. Do not open and re-save
  it, convert it, strip metadata or resize it. Read it only as raw bytes.
- If the photo is at a URL, download it as-is: `curl -sSL -o photo.jpg <url>`.
  Do not use a preview or thumbnail.
- If the photo was pasted into the chat as an image, you may only have a
  re-encoded copy. Ask for the file itself.
- If the person's copy went through a messaging app, social network or a
  screenshot, the signature is almost certainly gone. Say so and ask for the
  original file.

## 3. Run the verifier

```bash
python3 verify.py --json photo.jpg
```

Python 3 is all it needs. Useful variants:

- `--key <64 hex chars>` checks against one specific public key instead of
  the registry.
- `--registry <path or URL>` uses a different `devices.json`.

Before trusting a result on a surprising photo, you can confirm the tool
works: `sh tests/run.sh` should print four `ok` lines.

## 4. Interpret the result

The JSON contains `status`, and where they apply `device`,
`key_fingerprint`, `capture_time_utc`, `sha256`, `registry` and `reason`.

| `status` | Exit | Meaning | What to tell the person |
| --- | --- | --- | --- |
| `VERIFIED` | 0 | Signed by a registered camera, unchanged since. | Camera `device.id` and its description, the capture time, and the file's SHA-256. |
| `INVALID` | 1 | It has a camera signature, but the file changed after signing. | Treat it as altered. Quote `reason`. |
| `NOT_SIGNED` | 3 | No camera signature in the file. | Either not from a registered camera, or re-encoded on the way. Ask for the original. |
| `UNKNOWN_DEVICE` | 4 | Signed by a camera whose key is not registered here. | Origin cannot be confirmed. Give the `key_fingerprint`. |
| (none) | 2 | Tool or input error, message on stderr. | Fix the input and rerun. Do not report a verdict. |

Rules for the answer:

- Call the capture time "signed" only when the status is `VERIFIED`. Even
  then, it is the camera's own clock.
- `VERIFIED` is about the file, not the scene. Always add one sentence on the
  limits: in this demo the key is stored in the camera's flash, so someone
  with full access to that camera could copy it, and a camera photographing
  a screen still produces a valid signature.

Example answer for a verified photo:

> The photo is verified. It was signed by camera `sonoff-s2-demo-01`
> (Sonoff S2 demo camera) and has not been modified since. Signed capture
> time: 2026-10-31 18:44:16 UTC, by the camera's clock. SHA-256: `...`.
> This proves the file came from that camera unaltered; it doesn't prove the
> scene was genuine, and in this demo the camera's key could be copied by
> someone with full access to the device.

## 5. If you want a second opinion

`verify.py` uses its own small Ed25519 implementation so that it needs no
packages. `VALIDATION.md` ("Independent cross-check") has a 20-line script
that runs the same check with the `cryptography` library. If the two
disagree, report that instead of a verdict.

## 6. Without Python

Implement the steps in `VALIDATION.md` ("Validation steps") with any standard
Ed25519 library (libsodium, Go `crypto/ed25519`, Node
`crypto.verify(null, msg, key, sig)`, WebCrypto). Test your code on
`tests/sample-signed.jpg` (must verify) and `tests/sample-tampered.jpg`
(must fail) with `tests/test-keys.json` before using it on the real photo.

## Do not

- Do not edit `keys/devices.json` while verifying a photo. Registering
  cameras is a separate change a human approves.
- Do not treat `tests/test-keys.json` as a real camera. Its private key is
  public.
- Do not upload the photo to any service to check it.
