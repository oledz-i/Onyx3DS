# Installing ONYX on Xbox

This guide takes you from a console in retail mode to a game library in ONYX.

## 1. Put the console in Developer Mode

1. Register a Microsoft Partner Center developer account (one-time fee).
2. On the Xbox, install the **Xbox Dev Mode** app from the Store and follow its steps.
   Microsoft's full guide: [Xbox Developer Mode activation](https://learn.microsoft.com/windows/uwp/xbox-apps/devkit-activation).
3. When the console restarts into **Dev Home**, turn on **Remote Access** (Dev Home →
   *Remote Access Settings*) and note the Device Portal address shown there, e.g.
   `https://192.168.1.50:11443`.

## 2. Download ONYX

- From [Actions](https://github.com/oledz-i/Onyx3DS/actions/workflows/build.yml), open the
  latest successful run and download the **`ONYX3DS-xbox`** artifact (a GitHub login is
  needed for Actions downloads). Releases will be posted once ONYX is out of alpha.
- Unzip it on your PC. Inside `AppPackages\...` you'll find the `.msix` and a
  `Dependencies\x64` folder.

## 3. Install with the Device Portal

1. On your PC, open the Device Portal address in a browser and accept the certificate
   warning (the portal uses a self-signed certificate).
2. Under **Home → My games & apps**, choose **Add**.
3. Select the ONYX `.msix`, then on the next page add **every file** from
   `Dependencies\x64`, and start the install.
4. Updating later works the same way; your settings and saves are kept.

## 4. Set ONYX to run as a Game

In **Dev Home**, highlight ONYX, press the **Menu** button, choose **View details**, and
set **App type** to **Game**.

As an *App*, Xbox caps ONYX at a small amount of memory and some games will be
closed by the system without warning. As a *Game*, it gets several GB.

## 5. Prepare your files

Copy your games to a USB drive (exFAT or NTFS) or to a folder ONYX can browse. A tidy layout:

```
E:\ROMS\3DS\                 Games: .3ds .cci .cxi .3dsx ... (subfolders are scanned)
E:\ROMS\3DS\Updates\         Update and DLC .cia files
E:\ROMS\3DS\System\          aes_keys.txt, seeddb.bin, shared fonts (from your own 3DS)
E:\ROMS\3DS\Textures\        Texture packs: one folder per title ID, e.g. 00040000001B5000\
E:\ROMS\3DS\Mods\            LayeredFS mods: <TITLEID>\romfs\, exefs\ or code.ips
E:\ROMS\3DS\Cheats\          <TITLEID>.txt (downloads from the cheat database land here)
```

**Games must be decrypted.** Dump and decrypt them on your own 3DS with
[GodMode9](https://github.com/d0k3/GodMode9). ONYX flags encrypted dumps in the library;
it can't run them.

## 6. First launch

1. Start ONYX from Dev Home.
2. Choose your **Games** folder when asked. Other folders are under **Settings → Folders**.
3. For updates and DLC: put the `.cia` files in the *Updates & DLC* folder and use
   **Settings → Updates & DLC** to install them.
4. Optional: sign in to RetroAchievements and add a free SteamGridDB API key for box art
   under **Settings → Online services**.

## Recommended settings for a first test

| Setting | Value |
|---|---|
| Renderer | Software (safe) |
| CPU JIT | On |
| Shader JIT | Off for the very first boot, then On |
| System model | Original 3DS, unless the game needs a New 3DS |

## Logs

ONYX writes `onyx.log` (and `driver.log`) to its LocalState folder. In the Device Portal:
**File explorer → LocalAppData → ONYX3DS_… → LocalState**. Attach `onyx.log` to any bug
report; the last lines usually name the problem.
