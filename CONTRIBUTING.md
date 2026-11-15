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

## What to expect after you open a PR

1. **A first reply within a few days** saying we've seen it.
2. **We test it against the real game** on our own machines: we reproduce the bug first, then check your fix, and for
   anything visual we compare it with the game's original renderer. We post what we found on your PR, with
   before/after screenshots.
3. **If it needs changes**, we either ask you or add our own commits on top. Your commits stay exactly as you wrote
   them, so they keep your name and link to your GitHub account.
4. **It ships in the next release** that bundles it, and the release notes thank you. When that release reaches
   GitHub, your PR shows as merged.
5. **If we can't take it**, we explain why on the PR. If we reuse part of your idea or code in our own commit, that
   commit credits you as a co-author.

**What helps a PR go faster:** say what you saw (a screenshot of the bug, where in the game, your GPU, the render scale
or resolution), how you tested the fix, and keep each PR to one change.

