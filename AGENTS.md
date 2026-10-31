# Agent instructions: verify that a photo came from a registered camera

Use these steps when someone gives you a JPEG and asks whether it was taken by
one of the cameras listed in this repository. The format and the registry are
described in `README.md`.

## 1. Get the original bytes

- Work on the file exactly as the person provided it. Do not open and re-save
  it, convert it, strip metadata, resize it or let any tool "fix" it. Any such
  step destroys the signature.
- If the photo is at a URL, download it to a file as-is
  (`curl -sSL -o photo.jpg <url>`). Do not use a preview or thumbnail.
- If the person only has a copy that went through a chat app, email client,
  screenshot or social network, tell them the signature is very likely gone
  and ask for the original file.

## 2. Run the verifier

From a clone of this branch:

```bash
python3 verify.py --json photo.jpg
```

If you only have `verify.py` on its own, it downloads `keys/devices.json` from
the published branch. To check against one key someone gave you:
`python3 verify.py --json --key <64-hex> photo.jpg`.

It needs only Python 3; nothing to install. Do not substitute a different
verifier unless Python is unavailable (see step 5).

## 3. Read the result

The JSON has `status`, plus `device`, `key_fingerprint`, `capture_time_utc`,
`sha256` and `reason` where they apply.

| `status` | Exit | What to tell the person |
| --- | --- | --- |
| `VERIFIED` | 0 | The file is byte-for-byte what camera `device.id` signed. Give the device id and description, the capture time, and the SHA-256. |
| `INVALID` | 1 | It carries a camera signature, but the file was changed after signing. Treat it as tampered. Quote `reason`. |
| `NOT_SIGNED` | 3 | No camera signature. Either not from a registered camera, or re-encoded on the way. Ask for the original. |
| `UNKNOWN_DEVICE` | 4 | Signed by a camera key that is not registered here. Origin cannot be confirmed. Give the `key_fingerprint`. |
| (exit 2) | 2 | Tool or input error. Read stderr and fix the input; do not report a verdict. |

Say "unverified" for the capture time unless the status is `VERIFIED`. Even
then it is the camera's own clock.

## 4. State the limits

Always add, in one or two sentences, what a `VERIFIED` result does not prove:

- The key lives in the camera's flash in this demo, so someone with full
  access to that camera could copy it and sign other images.
- A signature does not show the scene was real (for example, a photo of a
  screen).

## 5. Without Python

Reimplement the check from the format in `README.md` ("How the camera signs"):

1. The file must end with `FF D9`. The 93 bytes before that must start with
   `FF EF 00 5B` followed by UUID bytes `b050bf0788564598869e44c4dc208f26`.
2. Byte 20 of that segment is the version (must be `01`), bytes 21-28 the
   key fingerprint, bytes 29-92 the Ed25519 signature.
3. Find the registry entry whose SHA-512(public key)[0:8] equals the
   fingerprint.
4. Verify the signature with that public key over the first `len - 95` bytes
   of the file: everything before the segment, without the segment and the
   final `FF D9`.

Use a standard Ed25519 implementation (for example `cryptography`, PyNaCl,
libsodium, Node's `crypto.verify(null, msg, key, sig)` or WebCrypto). Run
`sh tests/run.sh` or check your code against `tests/sample-signed.jpg`
(valid) and `tests/sample-tampered.jpg` (invalid) with `tests/test-keys.json`
first.

## Do not

- Do not add, remove or edit entries in `keys/devices.json` while verifying a
  photo. Registering cameras is a separate, human-approved change.
- Do not treat `tests/test-keys.json` as a real camera. Its private key is
  public.
- Do not upload the person's photo anywhere to verify it. Everything runs
  locally.
