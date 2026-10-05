Skullburglar
============

A native port of 50 Cent: Blood on the Sand (Xbox 360, 2009).

You need your own copy of the game. On first launch a setup window asks for your disc image (.iso) or an
extracted copy of the disc, and installs the game files to:

  Windows  %LOCALAPPDATA%\bots-recomp\game
  macOS    ~/Library/Application Support/bots-recomp/game
  Linux    ~/.local/share/bots-recomp/game

Saves and settings (bots.toml) are kept per user as well. Run with --launcher to open the settings
window before the game starts.

The first launch compiles shaders for your GPU, which can take from a few seconds to a few minutes.

Source, issues and building it yourself: https://github.com/nlwstein/skullburglar
