Skullburglar
============

A native port of 50 Cent: Blood on the Sand (Xbox 360, 2009) for Windows, Linux, the Steam Deck and macOS
(Apple silicon).

You need your own copy of the game: a disc image (.iso) or an extracted copy of the disc. Nothing from the game
is included here.


Installing
----------

Windows  Run the -setup.exe. It installs to C:\Games\Skullburglar (you can pick another folder) with a Start
         menu entry, and needs no administrator rights. If SmartScreen says it "protected your PC", choose
         More info, then Run anyway. Uninstall from Settings > Apps.

macOS    Open the .dmg and drag Skullburglar into Applications. The app is not notarized, so the first time
         macOS refuses to open it: open System Settings > Privacy & Security, scroll down to the message about
         Skullburglar and choose Open Anyway (or run: xattr -dr com.apple.quarantine /Applications/Skullburglar.app).
         Needs macOS 13.3 or later on an Apple silicon Mac.

Linux    Either the .AppImage (make it executable and run it) or the .tar.xz (unpack it and run ./bots).
         Needs Vulkan graphics drivers.

Steam Deck  Copy the .AppImage to the Deck, make it executable (Properties > Permissions in the file manager) and
            add it to Steam with "Add a Non-Steam Game". It runs in Game Mode. The launcher is a mouse window: use
            the touchscreen or the right trackpad (R2 clicks).


The launcher
------------

Every start opens the launcher. The first time it asks for your disc image or extracted disc and installs the
game files (a few minutes). After that it holds the settings: resolution, VSync, frame cap, fullscreen, intro
movies, renderer, mouse sensitivity and key bindings. Play saves them and starts the game.

When a newer version is out, the launcher says so and its Download button opens the download page (turn
"Check for new versions" off to stop it looking).

To skip the launcher and go straight into the game, start it with --launcher=false (on the Deck: put that in the
Steam shortcut's Launch Options).

The first start also compiles the game's shaders for your GPU, with a progress screen (seconds to about half a
minute). Later starts reuse them.


Playing
-------

A controller works as on the Xbox. Keyboard and mouse:

  Move          W A S D              Fire / aim        Left / right mouse button
  Cover, run    Space                Reload, pick up   R
  Special move  E or F               Gangsta Fury      Q
  Grenade       G or middle button   Next grenade      T
  Taunt         C                    Weapons           1-4, arrows, mouse wheel
  Objectives    Tab                  Pause             Escape
  Fullscreen    F11

Menus work with the mouse. Button prompts follow the device you last used. Keys can be changed in the launcher.
To quit, choose Quit on the main menu or close the window.


Co-op
-----

The story can be played by two people, three ways. All of them are in the launcher's Co-op section.

LAN co-op (two computers on the same network; on by default)
  1. Player 1 starts a story (Play Game, then New Story or a chapter).
  2. Player 2 picks Play Game, then Join COOP, chooses a sidekick and weapons, then Find Game.
  3. Player 1's game shows up in the Game List; select it to join. X refreshes the list.
  Windows, Linux, the Steam Deck and macOS can play together. On Windows, allow Skullburglar through the firewall
  the first time it asks, or games won't be found. If you dismissed that prompt: Windows Security > Firewall &
  network protection > Allow an app through firewall, and tick Private for Skullburglar.
  It uses UDP ports 24001 and 17777. If the list stays empty on a network that blocks broadcasts, start the game
  with --netplay_peers=<the other computer's IP address>. Turn LAN co-op off in the launcher if you don't want
  others on your network to see your story.

Split-screen co-op (one screen; experimental)
  Turn on "Split-screen co-op" in the launcher. During a story, press START on a second controller to join as
  player 2. Player 2 leaves with Quit in their pause menu.

Two windows on one PC (experimental)
  Turn on "Co-op on this PC, two windows". Player 2's window opens next to player 1's: player 1 starts a story,
  player 2 picks Play Game, then Join COOP. "Co-op controllers" picks which pad is whose.


Where things are kept
---------------------

Game files (installed by the launcher)
  Windows  %LOCALAPPDATA%\bots-recomp\game
  macOS    ~/Library/Application Support/bots-recomp/game
  Linux    ~/.local/share/bots-recomp/game

Saves, settings (bots.toml) and the shader cache
  Windows  Documents\bots
  macOS    ~/.local/share/bots
  Linux    ~/.local/share/bots

Logs
  Windows  logs\ in the install folder (and a crash-<number>.dmp if it crashes)
  macOS    ~/Library/Logs/Skullburglar
  Linux    logs/ next to bots; the AppImage writes ~/.local/state/bots-recomp/logs

Updating: on Windows run the new setup (it finds your install and updates it); elsewhere replace the AppImage,
folder or app with the new version from https://skullburglar.stz.sh. Game files, saves and settings stay.


Reporting a problem
-------------------

Open an issue at https://github.com/nlwstein/skullburglar/issues with your platform, GPU, what you were doing and
the newest log file (plus the .dmp on a Windows crash).


About
-----

Downloads and news: https://skullburglar.stz.sh. Source: https://github.com/nlwstein/skullburglar (BSD 3-Clause).
THIRD-PARTY-NOTICES.txt lists the other software included and its licenses.

An unofficial fan project, not affiliated with or endorsed by 50 Cent, the game's developer or publisher, or
Microsoft. All trademarks belong to their owners.
