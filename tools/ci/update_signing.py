"""The release signing rules shared by publish_downloads.py, check_signing_key.py and tests/update_signing_test.py.

Everything here must agree with the launcher's reader in src/setup/update_logic.cpp (PlainKey, PlainFileName, IsHex64,
PlainVersion, SigningMessage): one entry it rejects makes it treat the whole release as unsigned, which silently turns
"Update now" off for every platform. So the publisher applies the same rules before it signs and fails loudly.

No network and no boto3 here, so the preflight runs before anything is tagged.
"""
import base64
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519_sign  # noqa: E402

FORMAT = "skullburglar-update-v1"
MAX_SIZE = 2_000_000_000


def plain_version(v):
    return bool(re.fullmatch(r"[0-9A-Za-z.+-]{1,64}", v)) and bool(re.search(r"\d", v))


def plain_key(k):
    return bool(re.fullmatch(r"[a-z0-9_]{1,32}", k))


def plain_file_name(f):
    return bool(re.fullmatch(r"[A-Za-z0-9._-]{1,200}", f)) and not f.startswith(".")


def validate_release(version, downloads):
    """Raises ValueError naming the first thing the launcher would reject."""
    if not plain_version(version):
        raise ValueError(f"version {version!r} must be 1-64 characters of [0-9A-Za-z.+-] and contain a digit")
    if not downloads:
        raise ValueError("a release with no packages")
    for k, d in downloads.items():
        if not plain_key(k):
            raise ValueError(f"package key {k!r} must be 1-32 characters of [a-z0-9_]")
        if not plain_file_name(d["file"]):
            raise ValueError(f"file name {d['file']!r} must be 1-200 characters of [A-Za-z0-9._-], not starting with a dot")
        size = d["size"]
        if not isinstance(size, int) or isinstance(size, bool) or not 1 <= size <= MAX_SIZE:
            raise ValueError(f"size of {d['file']} ({size!r}) must be an integer from 1 to {MAX_SIZE}")
        if not re.fullmatch(r"[0-9a-f]{64}", d["sha256"]):
            raise ValueError(f"sha256 of {d['file']} must be 64 lowercase hex digits")


def signing_message(version, downloads):
    """The text the launcher verifies (src/setup/update_logic.cpp SigningMessage): the version, then one line per
    package, sorted: "key file size sha256". Validates first, so a release the launcher would reject never gets signed."""
    validate_release(version, downloads)
    lines = sorted(f"{k} {d['file']} {d['size']} {d['sha256']}\n" for k, d in downloads.items())
    return (FORMAT + "\nversion=" + version + "\n" + "".join(lines)).encode()


def built_in_public_key():
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "..", "packaging", "update-pubkey.txt")) as f:
        return next((l.strip().lower() for l in f if re.fullmatch(r"[0-9a-fA-F]{64}", l.strip())), "")


def signing_seed(environ=None, public_key_hex=None):
    """The signing key's 32-byte seed from UPDATE_SIGNING_KEY (base64, or 64 hex digits), or None when unset off the
    default branch. Exits (SystemExit with a message that never contains the key) when it's missing on the default
    branch, malformed, or doesn't match the built-in public key."""
    env = os.environ if environ is None else environ
    raw = env.get("UPDATE_SIGNING_KEY", "").strip()
    on_default_branch = bool(env.get("CI")) and env.get("CI_COMMIT_BRANCH") == env.get("CI_DEFAULT_BRANCH")
    if not raw:
        if on_default_branch:
            sys.exit("UPDATE_SIGNING_KEY is not set: refusing to publish an unsigned release from the default branch")
        return None
    try:
        seed = bytes.fromhex(raw) if re.fullmatch(r"[0-9a-fA-F]{64}", raw) else base64.b64decode(raw, validate=True)
    except ValueError:
        sys.exit("UPDATE_SIGNING_KEY is neither base64 nor hex")
    if len(seed) != 32:
        sys.exit("UPDATE_SIGNING_KEY must be a 32-byte seed")
    built_in = built_in_public_key() if public_key_hex is None else public_key_hex
    if built_in and ed25519_sign.public_key(seed).hex() != built_in:
        sys.exit("UPDATE_SIGNING_KEY doesn't match the public key in packaging/update-pubkey.txt")
    return seed
