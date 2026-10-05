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
folder or app with the new version. Game files, saves and settings stay.


Reporting a problem
-------------------

Send your platform, GPU, what you were doing and the newest log file (plus the .dmp on a Windows crash).
