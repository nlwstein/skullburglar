#!/usr/bin/env python3
"""RFC 8032 section 7.1 test vectors for tools/ci/ed25519_sign.py, and a cross-check against the `cryptography`
package when it is installed. Prints the same vectors as C++ for tests/update_test.cpp with --cpp."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "ci"))
import ed25519_sign as ed  # noqa: E402

# (seed, public key, message, signature), RFC 8032 7.1 tests 1-3.
VECTORS = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
]


def main():
    ok = True
    for seed, pk, msg, sig in VECTORS:
        s = bytes.fromhex(seed)
        got_pk = ed.public_key(s).hex()
        got_sig = ed.sign(s, bytes.fromhex(msg)).hex()
        if pk and got_pk != pk or got_sig != sig:
            print("MISMATCH", seed[:8], got_pk, got_sig)
            ok = False
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        for n in range(8):
            seed, msg = os.urandom(32), os.urandom(n * 37)
            k = Ed25519PrivateKey.from_private_bytes(seed)
            assert k.sign(msg) == ed.sign(seed, msg)
    except ImportError:
        print("cryptography not installed: cross-check skipped")
    print("ed25519 vectors:", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
