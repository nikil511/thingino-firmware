#!/usr/bin/env python3
"""Verify that a JPEG snapshot was signed by a registered Thingino camera.

Thingino's raptor streamer signs every /snap.jpg with the camera's
Ed25519 device key. The signature lives in an APP15 segment placed
immediately before the final EOI marker:

    FF EF                     APP15 marker
    00 5B                     segment length (91, excludes marker)
    B0 50 BF 07 88 56 45 98   raptor signing UUID (16 bytes)
    86 9E 44 C4 DC 20 8F 26
    01                        format version
    <8 bytes>                 key fingerprint = SHA-512(public_key)[0:8]
    <64 bytes>                Ed25519 signature
    FF D9                     EOI

The signed message is every byte of the file before the FF EF marker,
which includes the capture-time EXIF the camera inserts.

Pure Python 3 standard library; no packages to install.

Exit codes:
    0  VERIFIED        signature valid for a registered device
    1  INVALID         signature present but does not verify (tampered),
                       or moved away from the end of the file
    2  error           bad arguments, unreadable file, bad registry
    3  NOT_SIGNED      no raptor signature found
    4  UNKNOWN_DEVICE  well-formed signature, signer not in the registry
"""

import argparse
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

# Raw URL of keys/devices.json on the published branch. A local copy next
# to this script (a git clone of the branch) takes precedence.
REGISTRY_URL = "https://raw.githubusercontent.com/nikil511/thingino-firmware/device-keys/keys/devices.json"

SIGN_UUID = bytes.fromhex("b050bf0788564598869e44c4dc208f26")
SEGMENT_LEN = 93  # FF EF + 2-byte length + 16 uuid + 1 version + 8 fp + 64 sig
SIG_MARKER = b"\xff\xef\x00\x5b" + SIGN_UUID

# --- Ed25519 verification (RFC 8032, section 5.1.7) -----------------------

_P = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


def _decode_point(b):
    y = int.from_bytes(b, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % _P
    b = (p[1] + p[0]) * (q[1] + q[0]) % _P
    c = 2 * p[3] * q[3] * _D % _P
    d = 2 * p[2] * q[2] % _P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _same(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % _P == 0 and (p[1] * q[2] - q[1] * p[2]) % _P == 0


_GY = 4 * pow(5, _P - 2, _P) % _P
_G = (_recover_x(_GY, 0), _GY, 1, _recover_x(_GY, 0) * _GY % _P)


def ed25519_verify(public_key, message, signature):
    if len(public_key) != 32 or len(signature) != 64:
        return False
    a = _decode_point(public_key)
    r = _decode_point(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    h = int.from_bytes(hashlib.sha512(signature[:32] + public_key + message).digest(), "little") % _L
    return _same(_mul(s, _G), _add(r, _mul(h, a)))


# --- Photo parsing ----------------------------------------------------------


def fingerprint(public_key):
    return hashlib.sha512(public_key).digest()[:8].hex()


def parse_signature(data):
    """Return (message, version, fingerprint_hex, signature) or a reason string."""
    if data[:2] != b"\xff\xd8":
        return "not a JPEG file (no SOI marker)"
    end = len(data) - 2 - SEGMENT_LEN
    if data[-2:] == b"\xff\xd9" and end >= 2 and data[end:end + 20] == SIG_MARKER:
        seg = data[end:]
        return data[:end], seg[20], seg[21:29].hex(), seg[29:93]
    if data.find(SIG_MARKER) >= 0:
        return "a raptor signature segment exists but is not at the end of the file: bytes were added or removed after signing"
    return None


def capture_time(data):
    """Capture time from the EXIF block raptor writes right after SOI."""
    if data[2:4] != b"\xff\xe1" or data[6:12] != b"Exif\x00\x00":
        return None
    tiff = data[12:]
    try:
        stamp = tiff[68:87].decode("ascii")
        micros = int(tiff[95:101].decode("ascii"))
        t = datetime.strptime(stamp, "%Y:%m:%d %H:%M:%S").replace(tzinfo=timezone.utc)
    except (ValueError, UnicodeDecodeError):
        return None
    return t.replace(microsecond=micros).isoformat().replace("+00:00", "Z")


# --- Registry ---------------------------------------------------------------


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=15) as resp:
            return resp.read()
    except urllib.error.URLError as e:
        # python.org builds on macOS ship without CA certificates; curl
        # uses the system trust store instead.
        if not isinstance(e.reason, ssl.SSLError) or not shutil.which("curl"):
            raise
        done = subprocess.run(["curl", "-sSfL", "--max-time", "15", url],
                              capture_output=True)
        if done.returncode:
            raise OSError(f"curl: {done.stderr.decode(errors='replace').strip()}") from e
        return done.stdout


def load_registry(source):
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "keys", "devices.json")
    if source is None:
        source = here if os.path.exists(here) else REGISTRY_URL
    if source.startswith(("http://", "https://")):
        doc = json.loads(fetch(source))
    else:
        with open(source, encoding="utf-8") as f:
            doc = json.load(f)
    devices = []
    for dev in doc.get("devices", []):
        pub = bytes.fromhex(dev["public_key"])
        if len(pub) != 32:
            raise ValueError(f"device {dev.get('id')}: public_key must be 64 hex chars")
        dev = dict(dev, fingerprint=fingerprint(pub))
        devices.append(dev)
    return source, devices


# --- Main -------------------------------------------------------------------


def verify(path, registry_source, key_hex):
    with open(path, "rb") as f:
        data = f.read()

    result = {
        "file": path,
        "sha256": hashlib.sha256(data).hexdigest(),
        "capture_time_utc": capture_time(data),
    }

    parsed = parse_signature(data)
    if parsed is None:
        result.update(status="NOT_SIGNED", reason="no raptor signature segment found")
        return 3, result
    if isinstance(parsed, str):
        result.update(status="INVALID", reason=parsed)
        return 1, result
    message, version, fp, sig = parsed
    result["key_fingerprint"] = fp
    if version != 1:
        result.update(status="INVALID", reason=f"unsupported signature format version {version}")
        return 1, result

    if key_hex:
        candidates = [{"id": "(key given on command line)", "public_key": key_hex,
                       "fingerprint": fingerprint(bytes.fromhex(key_hex))}]
        result["registry"] = None
    else:
        source, devices = load_registry(registry_source)
        result["registry"] = source
        candidates = [d for d in devices if d["fingerprint"] == fp]
        if not candidates:
            result.update(status="UNKNOWN_DEVICE",
                          reason="signed by a key that is not in the registry")
            return 4, result

    for dev in candidates:
        if ed25519_verify(bytes.fromhex(dev["public_key"]), message, sig):
            result.update(status="VERIFIED", device=dev)
            return 0, result

    result.update(status="INVALID",
                  reason="signature does not match the image: it was modified after signing, or signed by a different key")
    return 1, result


def main():
    ap = argparse.ArgumentParser(description="Verify a Thingino/raptor signed JPEG snapshot.")
    ap.add_argument("photo", help="the original JPEG file, byte for byte as the camera produced it")
    ap.add_argument("--registry", help="path or URL of devices.json (default: local keys/ or the published branch)")
    ap.add_argument("--key", help="verify against this 64-hex Ed25519 public key instead of the registry")
    ap.add_argument("--json", action="store_true", help="print a machine-readable JSON result")
    args = ap.parse_args()

    if args.key and len(bytes.fromhex(args.key)) != 32:
        ap.error("--key must be 64 hex characters")
    try:
        code, result = verify(args.photo, args.registry, args.key)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
        return code

    print(f"file:             {result['file']}")
    print(f"sha256:           {result['sha256']}")
    if result.get("key_fingerprint"):
        print(f"key fingerprint:  {result['key_fingerprint']}")
    if result.get("capture_time_utc"):
        note = "camera clock, signed" if result["status"] == "VERIFIED" else "UNVERIFIED"
        print(f"capture time:     {result['capture_time_utc']} ({note})")
    if result.get("registry"):
        print(f"registry:         {result['registry']}")
    if result["status"] == "VERIFIED":
        dev = result["device"]
        print(f"device:           {dev['id']}" + (f" - {dev['description']}" if dev.get("description") else ""))
        print(f"public key:       {dev['public_key']}")
    else:
        print(f"reason:           {result['reason']}")
    print(f"RESULT: {result['status']}")
    return code


if __name__ == "__main__":
    sys.exit(main())
