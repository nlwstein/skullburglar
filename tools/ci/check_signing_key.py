#!/usr/bin/env python3
"""Release preflight: fails before anything is tagged or published when the release couldn't be signed.

    VERSION=0.4.0 python3 tools/ci/check_signing_key.py

Runs the checks publish_downloads.py makes at the end of the release (tools/ci/update_signing.py): the version is one
the launcher accepts, UPDATE_SIGNING_KEY is set on the default branch, and its public half is the one in
packaging/update-pubkey.txt. Prints only the verdict, never the key.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import update_signing  # noqa: E402


def main():
    version = os.environ.get("VERSION", "")
    if not update_signing.plain_version(version):
        sys.exit(f"VERSION {version!r} is not one the launcher accepts ([0-9A-Za-z.+-], at most 64 characters)")
    seed = update_signing.signing_seed()
    print("signing key ok" if seed else "no signing key (allowed off the default branch): the entry would be unsigned")


if __name__ == "__main__":
    main()
