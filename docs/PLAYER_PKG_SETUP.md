# Installing the Sakura Wars update and DLC

Sakura Wars PS4RT supports **CUSA16429 version 1.01**. If you have an
extracted version 1.00 game, the launcher can install your own matching
v1.01 update PKG. It can also install optional DLC.

**You do not need a separate PKG extractor or any development tools.**
The portable release includes everything needed for update and DLC setup.
Game files, updates, and DLC are **not** included.

## First launch

1. Extract the PS4RT Linux release and keep its files together.
2. Have your own **extracted, plaintext `CUSA16429` game folder** ready.
   Select the folder containing `eboot.bin` and `sce_sys/param.sfo`,
   not a PKG file or the folder above it.
3. From the extracted PS4RT release folder, launch:

   ```bash
   bash Play-Game.sh --game "/path/to/CUSA16429"
   ```

4. If your game is **already v1.01**, the launcher starts game preparation
   directly, with **no update or optional DLC prompts**.
   If it is **v1.00**, a setup window requires your own
   **matching CUSA16429 v1.01 update PKG**. Choose **Select PKG...**,
   then **Apply Update**, and let installation finish.
5. After the v1.00 update succeeds, an **optional DLC picker** opens.
   Select one or more compatible DLC PKGs, or choose **Close and launch game**
   to skip. After that, PS4RT restarts its launcher automatically and starts
   the updated game. You can also install DLC later through the in-game
   overlay or explicit CLI flags.

**Important: Applying the update changes the extracted `CUSA16429`
folder you selected.** Updated files replace the v1.00 files in that
folder. PS4RT does **not** automatically keep a backup of the original
v1.00 extraction. Make your own copy beforehand if you want to preserve
it for another application. Your selected update PKG is not changed.

The update is required to play; cancelling it on a v1.00 game does not
launch the unsupported version.

## Installing DLC later

DLC never interrupts normal game startup. Add it whenever you like:

1. Launch the game and open the PS4RT menu with **Insert** on a keyboard
   or **L2 + R3 click** on a controller.
2. Open **Game > Install DLC...** and select the DLC PKG files you own.
3. If new DLC was installed, **save your game**, then select **Restart to
   load DLC...** in the overlay and confirm. This restarts the game cleanly;
   unsaved gameplay progress will be lost. Cancelling DLC setup does not
   require a restart.

New DLC is loaded when the game starts again, **not while it is already
running**. Installed DLC is stored separately inside the PS4RT release
folder, under `out/CUSA16429/player-content/dlc/`. It does not
overwrite your base-game files.

## If something goes wrong

- **Update interrupted:** Launch again and select the **same v1.01
  update PKG** to finish or repair the update.
- **Update or DLC rejected:** Check that the files are for **CUSA16429**
  and are compatible with your game. Different-region or unsupported
  packages may not work.
- **DLC not appearing:** Save, then restart the game from the PS4RT
  menu. DLC is not loaded into an already-running session.
- **Need more help?** See the [main README](../README.md) or
  [report an issue](https://github.com/HekamiAeon/Sakura-Wars-PS4RT/issues).

PS4RT does not download games, updates, or DLC, or provide decryption
keys. Use your own legally obtained content.
