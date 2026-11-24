# Where Skullburglar keeps its files, and how to uninstall

**Short version:** Skullburglar keeps three things in three places: the **program** (100-200 MB), the **game files** the
launcher copied from your disc image (about 6.5 GB), and your **saves and settings** (under 1 MB, plus a shader cache).
Uninstalling the program leaves the other two on purpose, so a reinstall or update never costs you your progress. To
remove everything, follow [Uninstall completely](#uninstall-completely).

This page matches Skullburglar 1.15 and later. `~` means your home folder. On Windows, `%LOCALAPPDATA%` is the
`AppData\Local` folder in your user folder.

**Jump to:** [Find your files fast](#find-your-files-fast) ·
[What each folder is](#what-each-folder-is) ·
[Windows](#windows) · [Mac](#mac) · [Linux and Steam Deck](#linux-and-steam-deck) ·
[iPhone, iPad, Apple TV and Android](#iphone-ipad-apple-tv-and-android) ·
[Move to another drive](#move-to-another-drive) · [Back up your saves](#back-up-your-saves) ·
[Uninstall completely](#uninstall-completely)

## Find your files fast

- **The launcher tells you.** Open **Game files** to see "Installed in ..." for the game files. **About & logs** has
  **Open the log folder**.
- **Every log starts with the answer.** Open the newest log file and look for the lines `game folder:`, `saves:` and
  (in portable mode) `data folder:`. They give the exact paths this copy is using.
- **Where the logs are** is in the tables below. The macOS app and the Linux AppImage keep them outside the program,
  because the program itself is read-only.

## What each folder is

| Folder | What is in it | Size | Safe to delete? |
|---|---|---|---|
| **Program** | The game's program files | 100-200 MB | Yes. A reinstall brings it back. |
| **Game files** (`game`) | The files the launcher copied from your disc image | about 6.5 GB | Yes, but you need your disc image again to reinstall. Your saves are not in here. |
| **Saves and settings** (the "user" folder) | Your saves, your profile, your achievements and `bots.toml` (the launcher's settings) | under 1 MB | **No, unless you want to lose your progress.** `bots.toml` alone can go: settings go back to the defaults. |
| **Shader cache** (`cache`, inside the saves folder by default) | What the game prepared for your graphics card: shaders and the new renderer's meshes and textures | tens of MB up to about 1.2 GB | Yes. It is rebuilt on the next start, which takes a few minutes again. Backups skip it. |
| **Logs** | One text file per launch. Windows and the Linux tarball add `bots_001.log`, `bots_002.log`, ... and never delete old ones. The macOS app, the AppImage and portable mode write `bots-<date>-<time>.log` and keep the newest 10. Windows also writes `crash-<number>.dmp` after a crash. | tens of KB to a few MB each | Yes. Keep the newest one if you plan to report a problem. |
| **Save backups** | A copy of your saves folder, made every time you start the game (newest 10 kept, shader cache left out) | small | Yes, but they are your safety net. They also hold copies of your saves, so delete them for a full wipe. |
| **Crash archives** | The log (and on Windows the `.dmp`) of a session that crashed (newest 10 kept) | varies; a `.dmp` can be large | Yes. |
| **Player 2 folders** | Created only by "Co-op on this PC, two windows": a second copy of the saves folder and the shader cache for player 2 | small plus the cache | Yes. |
| **Update leftovers** | The "Update now" button downloads into the temporary folder and deletes it afterward. A failed update can leave a folder named `skullburglar-update-...` there, and on Windows two small notes (`update-pending.txt`, `update-staging.txt`) next to the game files | about the size of the download (30-60 MB) | Yes. The next start cleans old ones up by itself. |

The saves folder holds these entries. Two of them are named after the Xbox 360's ids, which is what the original game
uses:

| Entry | What it is |
|---|---|
| `B13EBABEBABEBABE` | Your saved games |
| `5451083B` | Your profile |
| `achievements` | Your achievements |
| `bots.toml` | The launcher's settings |
| `cache` | The shader cache (see above) |

## Windows

### Windows setup (the `-setup.exe`)

| What | Where | Removed by the uninstaller? |
|---|---|---|
| Program | `C:\Games\Skullburglar`, or the folder you picked in the setup | Yes, its own files. The folder stays if anything else is in it (see logs). |
| Logs and crash dumps | `logs` inside the program folder | **No.** This is why the folder can remain. |
| Start menu entry | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Skullburglar.lnk` | Yes |
| Registry | `HKEY_CURRENT_USER\Software\Skullburglar` (install folder and version) and `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\Skullburglar` (the entry in Settings > Apps) | Yes |
| Game files | `%LOCALAPPDATA%\bots-recomp\game` (or the folder you chose in the launcher) | **No** |
| Saves and settings | `Documents\bots` (see the note below) | **No** |
| Shader cache | `Documents\bots\cache` | **No** |
| Save backups | `Documents\bots-save-backups` | **No** |
| Crash archives | `Documents\bots-crashes` | **No** |
| Player 2 folders | `Documents\bots-p2` | **No** |
| Windows Firewall rules | Inbound rules named `bots.exe` or `Skullburglar`, made when you allowed network access for co-op | **No** |

The setup needs no administrator rights and makes no desktop shortcut and no other registry entries.

**Where is "Documents"?** It is the Documents folder of your user (usually `C:\Users\<you>\Documents`). If OneDrive backs
up your Documents, it is the one inside OneDrive. Open File Explorer, then Documents, to see the real place.

**Quick way to a folder:** press **Win + R**, type `%LOCALAPPDATA%\bots-recomp` and press Enter.

### Windows, portable

There is no separate portable download for Windows. To make a portable copy, run the setup, pick the folder you want
(for example `D:\Games\Skullburglar`), then follow [Portable mode](#portable-mode-keep-everything-in-one-folder).
Everything then lives in that one folder, and the Windows entries from the table above (Start menu, registry) are the
only things outside it. The registry and the Start menu entry point at the folder you picked.

## Mac

| What | Where | Removed by dragging the app to the Trash? |
|---|---|---|
| Program | `Skullburglar.app`, usually in `/Applications` | Yes |
| Game files | `~/Library/Application Support/bots-recomp/game` (or the folder you chose) | **No** |
| Saves and settings | `~/.local/share/bots` | **No** |
| Shader cache | `~/.local/share/bots/cache` | **No** |
| Save backups | `~/.local/share/bots-save-backups` | **No** |
| Crash archives | `~/.local/share/bots-crashes` | **No** |
| Player 2 folders | `~/.local/share/bots-p2` | **No** |
| Logs | `~/Library/Logs/Skullburglar` | **No** |
| Metal shader cache | `/var/folders/.../C/com.apple.metal`, kept by macOS for every Metal program | **No**, and macOS cleans it itself |
| A tiny preferences file | `~/Library/Preferences/org.skullburglar.Skullburglar.plist` (one setting that turns off the "hold a key for accents" popup in the game) | **No** |

**macOS hides these folders.** In Finder choose **Go > Go to Folder...** (Shift-Command-G) and paste the path. The
saves are in `~/.local/share` rather than `~/Library` on purpose: it is the same place on Mac and Linux.

Nothing is ever written inside `Skullburglar.app` itself. The disk image you downloaded (`.dmg`) is just the download:
eject it and delete it.

## Linux and Steam Deck

| What | AppImage and Steam Deck | Unpacked `.tar.xz` |
|---|---|---|
| Program | The `.AppImage` file wherever you put it | The folder you unpacked (it also holds `bots`, `logs`, `pipelines`) |
| Game files | `~/.local/share/bots-recomp/game` (or the folder you chose) | the same |
| Saves and settings | `~/.local/share/bots` | the same |
| Shader cache | `~/.local/share/bots/cache` | the same |
| Save backups | `~/.local/share/bots-save-backups` | the same |
| Crash archives | `~/.local/share/bots-crashes` | the same |
| Player 2 folders | `~/.local/share/bots-p2` | the same |
| Logs | `~/.local/state/bots-recomp/logs` | `logs` inside the unpacked folder |

If you set `XDG_DATA_HOME` or `XDG_STATE_HOME`, those replace `~/.local/share` and `~/.local/state`. The AppImage
unpacks nothing onto your disk: it is one file.

**Steam Deck:** it is the AppImage on Linux, so the table above applies. In Desktop Mode, press **Ctrl + H** in
Dolphin to show hidden folders like `.local`. Skullburglar does not create Steam shortcuts: the entry in your library is
the one you made with **Add to Steam** (or **Add a Non-Steam Game**), and you remove it in Steam. Steam also keeps a
shader cache for that shortcut (`steamapps/shadercache/<number>` in the library the game's compatibility data is in, if you use
Proton, or none for a native game): it is Steam's, not Skullburglar's, and Steam removes it with the shortcut.
If you chose an SD card or another folder for the game files in the launcher, the `game` folder is there instead.

## iPhone, iPad, Apple TV and Android

These builds are experimental. They have no launcher and keep everything inside the app's own storage, so
**deleting the app deletes the game files, saves and logs with it.** Copy your saves out first if you want to keep them.

| Device | Where everything is | Notes |
|---|---|---|
| **iPhone and iPad** | The app's **Documents** folder: `game` (the game files), `user` (saves and settings) and `logs`. The shader cache is in the app's `Library/Caches/bots`. | Documents shows in the Files app under **On My iPhone > Skullburglar**, and in Finder on a Mac. The game files are kept out of device backups; your saves are not. |
| **Apple TV** | The app's **Caches** folder holds `game`, `user` and `logs`, and the shader cache in `bots` | tvOS can clear its cache folders when the Apple TV runs low on space. The app then asks for the game files again, and **saves kept there can go too**. There is no way to copy them off. |
| **Android** | `Android/data/sh.stz.skullburglar/files` on the device's shared storage: `game`, `user` and `logs`. The shader cache is in the app's private storage. | A computer can reach that folder over USB. Some Android versions keep file-manager apps out of `Android/data`. |

**To uninstall:** iPhone, iPad and Apple TV: touch and hold the app, then choose **Remove App > Delete App**.
Android: Settings > Apps > Skullburglar > **Uninstall**. If you left your disc image in the app's folder or in
Downloads, it is still yours: delete it if you no longer need it.

## Move to another drive

### Only the game files (the big part)

The game files are about 6.5 GB, so this is usually all you need.

1. Open the launcher and go to **Game files**.
2. Choose **Reinstall from a disc image**, then **Choose another folder...** and pick a folder on the other drive.
   The files go in a folder named `game` inside the one you pick. (If the folder you pick already holds the game files,
   the launcher uses it as it is.)
3. Press **Install**. The launcher remembers the choice.

**To move what you already have instead of installing again:** close the game, move (cut and paste) the `game` folder
to the other drive, then in the launcher choose **Choose another folder...** and pick the folder that holds `game` (or
`game` itself). The choice is saved once the launcher finds the game files there. **Use the default folder** goes back.
When the old copy is no longer used, the launcher says so; delete it to free the space.

The macOS app and the Linux AppImage are read-only, so this is also the way to put game files anywhere but the
default.

### Everything: portable mode (keep it all in one folder)

Portable mode keeps your saves, settings, shader cache, logs and game files in one folder, so you can move it to
another drive or a USB stick.

1. **Install normally.** Put the program where you want it. Windows: pick the folder in the setup.
2. **Create an empty file named exactly `portable.txt`** next to the program:

   | Download | Put `portable.txt` next to | Everything goes in |
   |---|---|---|
   | Windows setup | `bots.exe` (the install folder) | `bots-data` in the install folder |
   | Linux `.tar.xz` | `bots` | `bots-data` in the unpacked folder |
   | Linux `.AppImage` and Steam Deck | the `.AppImage` file itself | `bots-data` next to the `.AppImage` |
   | Mac | `Skullburglar.app`, in the same folder | `bots-data` next to the app (never inside it) |

   - **Windows:** open Command Prompt in that folder and run `type nul > portable.txt`. Or turn on **View > Show >
     File name extensions** in File Explorer first: a "New Text Document" renamed `portable.txt` is otherwise really
     `portable.txt.txt` and does nothing. The launcher warns you when it sees that mistake.
   - **Mac and Linux:** in Terminal, `touch portable.txt` in that folder.
3. **Start the game.** `bots-data` appears next to `portable.txt` with these folders: `user` (saves and settings),
   `cache`, `logs`, `game` (the game files), `save-backups` and `crashes`. Delete `portable.txt` and the game goes back to
   the normal folders.

**On an SD card or USB drive, the game offers this itself.** The first time you start Skullburglar from removable or
external storage (the Steam Deck's SD card, a USB drive) on a computer that has no Skullburglar install yet, the launcher
asks **Keep everything on this drive?** **Yes** creates `portable.txt` next to the program for you and restarts the game
into portable mode, so the game files, your saves and your settings all live on the drive. Your saves are then on the
drive too: keep a copy of `bots-data\user` somewhere safe. **No** keeps the normal folders and never asks again. **Esc** (or
**B** on a controller) decides next time. The answer is a file next to the program: `portable.txt` for Yes,
`portable-declined.txt` for No (delete it to be asked again). A copy on internal storage, or on a computer that already has an
install, is never asked.

**Your existing game files (1.13.1 and later):** if this computer already has them from a normal install, the first
portable start offers to **copy** them into `bots-data\game`. It shows the size and your free space, you can cancel, and
the original stays where it is. **Not now** is remembered and the game keeps using the shared copy. You can copy later
from **Game files > Copy from the shared folder...**, or choose **Use it where it is** to keep one shared copy outside
the portable folder.

**Your existing saves do not move by themselves.** Close the game and copy what is inside your normal saves folder (see
the tables above) into `bots-data\user`. Copy the contents, not the folder: `bots-data\user` should then hold
`B13EBABEBABEBABE`, `5451083B` and `bots.toml`. If you copy them into `bots-data` itself by mistake, the launcher notices
and offers **Move them to the user folder**.

**The folder has to be writable.** If it is not (a read-only drive, a protected folder), the game says so and stops. It
never saves somewhere else without telling you.

**Portable details**
- The game looks for `portable.txt` next to the program you start, not the folder a shortcut or Steam starts it from,
  so shortcuts and Steam entries keep working.
- On a Mac, an app run straight from the Downloads folder may not see `portable.txt`. Drag it somewhere first.
- A crash dump (Windows) and the crash archive go in `bots-data\logs` and `bots-data\crashes`. On macOS and Linux a
  portable copy that crashes ends quietly instead of letting the system write its own crash report outside the folder.
- **Another folder of your choice:** set the environment variable `BOTS_DATA_DIR` to it (in Steam Launch Options:
  `BOTS_DATA_DIR=/mnt/games/bots %command%`). It beats `portable.txt` and uses the same layout.
- The Windows setup, "Update now" and the Linux updater leave `portable.txt` and `bots-data` alone.

**What portable mode can't move.** Portable mode writes nothing outside its folder, with these exceptions that belong to
the system, not to Skullburglar:

| What | Where | Why it stays |
|---|---|---|
| macOS Metal shader cache | `/var/folders/.../C/com.apple.metal` (the per-user cache folder) | Apple's graphics runtime keeps it for every Metal program, and nothing moves it. macOS cleans it itself. |
| Windows graphics driver caches | `%LOCALAPPDATA%\NVIDIA\DXCache`, `%LOCALAPPDATA%\AMD\VkCache`, `%LOCALAPPDATA%\D3DSCache` | The driver keeps them, for every game. |
| Windows Firewall rules | Inbound rules for the program | Windows writes them when you allow network access for co-op, or when you press **Fix it** in the launcher. |
| Steam's shader cache for the shortcut | `steamapps/shadercache/<shortcut id>` in your Steam library | Steam makes it for every non-Steam shortcut; Skullburglar doesn't write it. |
| Steam's shortcut itself | `shortcuts.vdf` in your Steam user folder | It is the entry you made with **Add to Steam**. |
| Windows setup program | Start menu entry and the two registry entries | A setup install is an install: the setup program writes them. Portable mode switches the launcher's **Update now** off for it (the setup program would write outside the folder); download the new version instead. |

On Linux, Mesa and NVIDIA shader caches go to `bots-data\cache\drivers` in portable mode, and temporary files (update
downloads) go to `bots-data\tmp`.

## Back up your saves

The game already makes a copy at every start: look in the save backups folder from the tables above (in portable mode,
`bots-data\save-backups`). The newest 10 are kept. For a backup of your own:

1. **Quit the game.**
2. **Copy your saves folder** to a safe place (a USB stick, cloud storage). You can leave out `cache`: it is big and
   rebuilds itself.
   - Windows: `Documents\bots`
   - Mac, Linux and Steam Deck: `~/.local/share/bots`
   - Portable: `bots-data\user`
3. **To restore:** quit the game, then copy the contents of the backup (yours, or a folder from the automatic backups)
   back into the saves folder, replacing what is there.

Saves from before an update or a reinstall are never touched by either. Only deleting the saves folder (or, on phones
and TVs, deleting the app) removes them.

## Uninstall completely

**Before you start:** if you might play again, [back up your saves](#back-up-your-saves) and skip the steps marked
**saves**. The game files are the easy part to remove: they only come back from your disc image.

### Windows setup

1. **Quit the game.** If you want to keep your progress, back up `Documents\bots` now.
2. **Uninstall the program:** Settings > Apps > Installed apps > Skullburglar > **Uninstall**. (The Start menu entry's
   right-click menu works too, and so does running `uninstall.exe` in the program folder.) This removes the program
   files, the Start menu entry and the two registry entries.
3. **Delete the program folder** if it is still there: `C:\Games\Skullburglar` (or the folder you picked). It stays
   when it still holds `logs`, a crash dump, `portable.txt` or `bots-data`. Check that nothing of yours is in it first.
4. **Delete the game files:** `%LOCALAPPDATA%\bots-recomp` (about 6.5 GB). If you chose another folder in the launcher,
   delete that `game` folder as well.
5. **Saves:** delete `Documents\bots` (your saves, settings and shader cache), then `Documents\bots-save-backups`,
   `Documents\bots-crashes` and, if it exists, `Documents\bots-p2`. After this your progress is gone.
6. **Firewall rules (optional):** open **Windows Defender Firewall with Advanced Security > Inbound Rules** and delete
   the rules named `bots.exe` or `Skullburglar` whose program path is the Skullburglar folder (it needs administrator
   permission). Leaving them does no harm.

**Keeping your saves:** do steps 1-4 and 6, and leave step 5 alone. A later install finds them again.

### Windows portable

Quit the game and delete the folder that holds `portable.txt` and `bots-data` (everything is in it), then do steps 2
and 6 above for the Start menu entry and the registry. Do the [Documents](#windows) cleanup too if you ever ran the
game without `portable.txt`.

### Mac

1. **Quit the game.** Back up `~/.local/share/bots` if you want your progress.
2. **Drag `Skullburglar.app` to the Trash** and empty it. Eject and delete the `.dmg` you downloaded.
3. **Delete the game files:** `~/Library/Application Support/bots-recomp` (about 6.5 GB, or the `game` folder you
   chose in the launcher).
4. **Delete the logs:** `~/Library/Logs/Skullburglar`.
5. **Delete the preferences file:** `~/Library/Preferences/org.skullburglar.Skullburglar.plist`.
6. **Saves:** delete `~/.local/share/bots`, then `~/.local/share/bots-save-backups`, `~/.local/share/bots-crashes`
   and `~/.local/share/bots-p2` if they exist.

In Finder use **Go > Go to Folder...** (Shift-Command-G) to open each path; `Library` and `.local` are hidden. For
portable mode, also delete `bots-data` next to the app.

**Keeping your saves:** do steps 1-5 and leave step 6.

### Linux (AppImage or `.tar.xz`)

1. **Quit the game.** Back up `~/.local/share/bots` if you want your progress.
2. **Delete the program:** the `.AppImage` file, or the whole unpacked folder (that also removes its `logs`). If an
   AppImage manager added a menu entry, remove it there too.
3. **Delete the game files:** `~/.local/share/bots-recomp` (about 6.5 GB, or the `game` folder you chose).
4. **Delete the AppImage's logs:** `~/.local/state/bots-recomp`.
5. **Saves:** delete `~/.local/share/bots`, then `~/.local/share/bots-save-backups`, `~/.local/share/bots-crashes`
   and `~/.local/share/bots-p2` if they exist.

For portable mode, also delete `bots-data` next to the program. **Keeping your saves:** do steps 1-4.

### Steam Deck

1. In **Game Mode**, find Skullburglar in your library (under **Non-Steam**), press the gear icon, then **Manage >
   Remove non-Steam game from your library**. If you made the entry another way, remove it in Steam the same way.
2. Switch to **Desktop Mode**, and press **Ctrl + H** in Dolphin so hidden folders show.
3. Do the Linux steps above: delete the `.AppImage` (for example `~/Games/Skullburglar.AppImage`),
   `~/.local/share/bots-recomp`, `~/.local/state/bots-recomp`, and, to lose your progress,
   `~/.local/share/bots` with its `-save-backups`, `-crashes` and `-p2` folders.

Skullburglar makes no other entries on the Deck. If you chose an SD card for the game files, delete the `game` folder
there too.

### iPhone, iPad, Apple TV, Android

Delete the app (see [above](#iphone-ipad-apple-tv-and-android)). That removes the game files, saves and logs with it, so
copy the `user` folder out first if you want to keep your progress (iPhone and iPad: Files app or Finder; Android: over
USB from `Android/data/sh.stz.skullburglar/files`; not possible on Apple TV).

### Check that nothing is left

Look at the tables for your system. After a full uninstall none of the paths in them should exist. The shader caches of
your graphics driver (kept by Windows, macOS or Mesa, not by Skullburglar) can stay: they belong to the driver and clean
themselves up.

## Still stuck?

[Open an issue](https://github.com/nlwstein/skullburglar/issues) with your system, your version and the newest log
file. The log's first lines list the folders in use.
