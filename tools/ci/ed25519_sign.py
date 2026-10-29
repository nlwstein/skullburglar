#!/usr/bin/env python3
"""Ed25519 (RFC 8032) signing without dependencies, for the release job and the update tests.

    python3 tools/ci/ed25519_sign.py keygen            prints a new "<seed hex> <public key hex>" (tests only)
    python3 tools/ci/ed25519_sign.py public            prints the public key
    python3 tools/ci/ed25519_sign.py sign <message file>   prints the signature as hex

`public` and `sign` read the seed (64 hex digits) from the ED25519_SEED environment variable, or from stdin when it is
unset; never from the command line, where `ps` and shell history would show it.

The launcher (src/setup/ed25519.cpp) verifies what this signs. This is the RFC's reference algorithm: slow (about a
tenth of a second per signature), which doesn't matter for one signature per release. Checked against the RFC's test
vectors in tests/ed25519_vectors.py.
"""
import hashlib
import os
import sys

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, -1, P) % P
I = pow(2, (P - 1) // 4, P)


def _xrecover(y):
    x2 = (y * y - 1) * pow(D * y * y + 1, -1, P) % P
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * I % P
    return P - x if x % 2 else x


BY = 4 * pow(5, -1, P) % P
B = (_xrecover(BY), BY, 1, _xrecover(BY) * BY % P)


def _add(p, q):
    a, b = (p[1] - p[0]) * (q[1] - q[0]) % P, (p[1] + p[0]) * (q[1] + q[0]) % P
    c, d = 2 * p[3] * q[3] * D % P, 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _compress(p):
    zi = pow(p[2], -1, P)
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _clamp(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    a, _ = _clamp(seed)
    return _compress(_mul(a, B))


def sign(seed, msg):
    a, prefix = _clamp(seed)
    pk = _compress(_mul(a, B))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % L
    rs = _compress(_mul(r, B))
    k = int.from_bytes(hashlib.sha512(rs + pk + msg).digest(), "little") % L
    return rs + ((r + k * a) % L).to_bytes(32, "little")


def _seed_arg():
    raw = (os.environ.get("ED25519_SEED") or sys.stdin.readline()).strip()
    try:
        seed = bytes.fromhex(raw)
    except ValueError:
        seed = b""
    if len(seed) != 32:
        sys.exit("the seed must be 64 hex digits, in ED25519_SEED or on stdin")
    return seed


def main(argv):
    if len(argv) == 2 and argv[1] == "keygen":
        seed = os.urandom(32)
        print(seed.hex(), public_key(seed).hex())
    elif len(argv) == 2 and argv[1] == "public":
        print(public_key(_seed_arg()).hex())
    elif len(argv) == 3 and argv[1] == "sign":
        print(sign(_seed_arg(), open(argv[2], "rb").read()).hex())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
