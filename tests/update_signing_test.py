#!/usr/bin/env python3
"""Tests for tools/ci/update_signing.py (the rules the release job applies before signing), the key preflight and the
ed25519_sign.py command line. Run: python3 tests/update_signing_test.py. Throwaway keys only."""
import base64
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.join(HERE, "..", "tools", "ci")
sys.path.insert(0, CI)
import ed25519_sign  # noqa: E402
import update_signing as us  # noqa: E402

SHA = "a" * 64
SEED = bytes(range(32))
PUB = ed25519_sign.public_key(SEED).hex()


def dl(**over):
    d = {"file": "skullburglar-1.2.0-macos-arm64.dmg", "size": 1234, "sha256": SHA}
    d.update(over)
    return d


class Rules(unittest.TestCase):
    def test_good_release(self):
        us.validate_release("1.2.0-rc.1+b5", {"windows": dl(), "linux_appimage": dl(), "ios": dl()})

    def test_versions(self):
        for ok in ("1.2.0", "0.0.0-dev.abc123", "1.2.0+build-5", "10"):
            self.assertTrue(us.plain_version(ok), ok)
        for bad in ("", "1.2 0", "1.2.0\n", "1.2.0/../x", "1.2.0_rc", "vé 1", "x" * 65, "abc"):
            self.assertFalse(us.plain_version(bad), repr(bad))

    def test_keys(self):
        for bad in ("", "Windows", "a-b", "a b", "x" * 33, "kéy"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                us.validate_release("1.0.0", {bad: dl()})

    def test_file_names(self):
        for bad in ("", ".hidden", "a/b", "a b", "a\nb", "x" * 201, "café.dmg", "a..b/../c"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                us.validate_release("1.0.0", {"windows": dl(file=bad)})

    def test_sizes_and_hashes(self):
        for bad in (0, -1, 2_000_000_001, 1.5, "10", True, None):
            with self.assertRaises(ValueError, msg=repr(bad)):
                us.validate_release("1.0.0", {"windows": dl(size=bad)})
        for bad in ("", "A" * 64, "a" * 63, "g" * 64, "a" * 65):
            with self.assertRaises(ValueError, msg=repr(bad)):
                us.validate_release("1.0.0", {"windows": dl(sha256=bad)})

    def test_empty_and_bad_version(self):
        with self.assertRaises(ValueError):
            us.validate_release("1.0.0", {})
        with self.assertRaises(ValueError):
            us.validate_release("1.0 .0", {"windows": dl()})

    def test_one_bad_entry_refuses_the_whole_release(self):
        with self.assertRaises(ValueError):
            us.signing_message("1.0.0", {"windows": dl(), "ios": dl(file="bad name.ipa")})

    def test_message_format_matches_launcher(self):
        m = us.signing_message("1.2.0", {"windows": dl(file="a", size=1, sha256="x" * 0 + SHA),
                                         "macos": dl(file="b", size=2)})
        self.assertEqual(m, f"skullburglar-update-v1\nversion=1.2.0\nmacos b 2 {SHA}\nwindows a 1 {SHA}\n".encode())


class Seed(unittest.TestCase):
    def env(self, **kw):
        return {"CI": "true", "CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main", **kw}

    def exits(self, env, pub=PUB):
        with self.assertRaises(SystemExit) as cm:
            us.signing_seed(env, pub)
        return str(cm.exception)

    def test_missing_on_default_branch_fails_elsewhere_allowed(self):
        self.assertIn("not set", self.exits(self.env()))
        self.assertIsNone(us.signing_seed(self.env(CI_COMMIT_BRANCH="feat/x"), PUB))
        self.assertIsNone(us.signing_seed({}, PUB))

    def test_good_key_base64_and_hex(self):
        self.assertEqual(us.signing_seed(self.env(UPDATE_SIGNING_KEY=base64.b64encode(SEED).decode()), PUB), SEED)
        self.assertEqual(us.signing_seed(self.env(UPDATE_SIGNING_KEY=SEED.hex()), PUB), SEED)

    def test_wrong_key_and_malformed_never_echo_the_key(self):
        other = bytes(reversed(SEED))
        for raw in (base64.b64encode(other).decode(), other.hex()):
            msg = self.exits(self.env(UPDATE_SIGNING_KEY=raw))
            self.assertIn("doesn't match", msg)
            self.assertNotIn(raw, msg)
        for raw in ("not base64!!", base64.b64encode(b"short").decode()):
            msg = self.exits(self.env(UPDATE_SIGNING_KEY=raw))
            self.assertNotIn(raw, msg)

    def test_preflight_script(self):
        script = os.path.join(CI, "check_signing_key.py")
        base = {k: v for k, v in os.environ.items() if k not in ("UPDATE_SIGNING_KEY", "CI", "CI_COMMIT_BRANCH", "CI_DEFAULT_BRANCH")}
        base.update(CI="true", CI_COMMIT_BRANCH="main", CI_DEFAULT_BRANCH="main", VERSION="1.2.0")

        def run(**kw):
            return subprocess.run([sys.executable, script], env={**base, **kw}, capture_output=True, text=True)

        self.assertNotEqual(run().returncode, 0)  # no key on the default branch
        self.assertNotEqual(run(VERSION="1.2 0", UPDATE_SIGNING_KEY=SEED.hex()).returncode, 0)
        wrong = run(UPDATE_SIGNING_KEY=bytes(reversed(SEED)).hex())  # not the repository's built-in key either
        self.assertNotEqual(wrong.returncode, 0)
        self.assertNotIn(bytes(reversed(SEED)).hex(), wrong.stdout + wrong.stderr)


class SignCli(unittest.TestCase):
    def run_cli(self, *args, env=None, stdin=""):
        e = {k: v for k, v in os.environ.items() if k != "ED25519_SEED"}
        e.update(env or {})
        return subprocess.run([sys.executable, os.path.join(CI, "ed25519_sign.py"), *args], env=e, input=stdin,
                              capture_output=True, text=True)

    def test_seed_from_env_or_stdin_not_argv(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(b"hello")
        try:
            want = ed25519_sign.sign(SEED, b"hello").hex()
            self.assertEqual(self.run_cli("sign", f.name, env={"ED25519_SEED": SEED.hex()}).stdout.strip(), want)
            self.assertEqual(self.run_cli("sign", f.name, stdin=SEED.hex() + "\n").stdout.strip(), want)
            self.assertEqual(self.run_cli("public", stdin=SEED.hex()).stdout.strip(), PUB)
            # The old form, with the seed on the command line, is refused.
            self.assertNotEqual(self.run_cli("sign", SEED.hex(), f.name).returncode, 0)
            self.assertNotEqual(self.run_cli("sign", f.name, stdin="zz").returncode, 0)
        finally:
            os.unlink(f.name)


if __name__ == "__main__":
    unittest.main()
