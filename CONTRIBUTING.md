# Contributing

Thanks for helping. A few rules keep this project publishable.

## Never include game data

No game files, disc images, extracted assets, save files, shader caches (`.xsh`, `.xpso`), screenshots of
copyrighted content, decompiled UnrealScript dumps, or generated C++ from `generated/`. Those are derived
from the game and can't be distributed. Issues and PRs that include them will be closed and the content
removed. The git hooks below block the common cases.

## Set up the hooks

```bash
git config core.hooksPath .githooks
```

They run `tools/leak_scan.py` on every commit and push: they block game file types, files over 1 MB,
secret-looking strings, and entries in `.githooks/blocklist.txt`.

## Commits

- [Conventional commits](https://www.conventionalcommits.org/): `fix(gpu): ...`, `feat(setup): ...`.
- Sign off each commit (`git commit -s`). This certifies the
  [Developer Certificate of Origin](https://developercertificate.org/): you wrote the change or have the
  right to submit it under this project's license.
- Describe the change, not where you tested it. Hostnames and personal paths don't belong in commits.

## How PRs land

The project is developed in a private repository that mirrors to GitHub. Your PR is fetched, reviewed,
built against a real copy of the game, and merged there with your commits and authorship kept as they are.
The mirror then shows it as merged here. Fixes on top of your PR are added as new commits; your commits
aren't rewritten.

Please don't expect a PR to be merged with GitHub's merge button: the mirror would overwrite it.
