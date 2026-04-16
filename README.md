# BetterSaving -- StarRupture Plugin

Replaces the default blocking save system in [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/) with an asynchronous pipeline that moves JSON serialisation, compression, and Steam Cloud I/O off the game thread, eliminating save-induced frame hitches.

**Target:** Game client and server

---

## What It Does

The vanilla save path calls `UCrSaveSubsystem::SaveGameInternal` on the game thread, which blocks while it serialises, compresses, and writes save data to Steam Cloud. On large save files this causes a noticeable freeze.

BetterSaving hooks `SaveGameInternal` and replaces the blocking work with a background thread pipeline:

| Phase | Where it runs | What happens |
|---|---|---|
| Metadata update | Game thread | `OnPreSaveStart` delegate fired, level path, world times, timestamp, and game version written into `FCrSaveGameData` |
| Meta file write | Game thread | `UCrSaveGameUtils::WriteMetaFile` called (mirrors original) |
| Data snapshot | Game thread | Raw `FCrSaveGameData` copied to a task struct |
| JSON serialisation | Background thread | `FJsonObjectConverter::UStructToJsonObjectString` converts the snapshot |
| Compression | Background thread | `FCompression::CompressMemory` (Oodle/Zlib) produces a compact payload with a 4-byte LE uncompressed-size header |
| Steam Cloud write | Background thread | `ISteamRemoteStorage::FileWrite` writes the compressed payload |
| Post-save | Background thread | `UCrSaveGameUtils::SaveLastSaveGameName` records the slot name |

On client builds a small on-screen widget shows the current save stage (Queued -> Compressing -> Writing -> Done / Failed).

---

## Configuration

Config is stored in `Plugins\config\BetterSaving.ini` and is generated on first launch.

| Section | Key | Default | Description |
|---|---|---|---|
| `General` | `Enabled` | `true` | `true` or `false` -- enables the plugin |
| `UI` | `ShowSaveWidget` | `true` | Show the on-screen save progress widget (client only) |

The save widget visibility can also be toggled at runtime via the in-game mod settings panel.

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page:
   - **Client:** `BetterSaving_Plugin-Client-*.zip`
   - **Server:** `BetterSaving_Plugin-Server-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder -- it will sit alongside your existing `dwmapi.dll`.

3. After the first launch, edit `Plugins\config\BetterSaving.ini` and confirm `Enabled=true`.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Save widget not visible | Confirm `ShowSaveWidget=true` in `BetterSaving.ini`, or toggle it in the mod settings panel. |
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Save fails / `Failed` shown | Check `modloader.log` for `[thread]` error lines. Usually caused by Steam not being running or a pattern mismatch after a game update. |
| Game updated, plugin broken | The hook uses byte-pattern scanning. A game update may shift the patterns -- wait for a plugin update. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK).

Clone the repo, open the `.sln` file, and build one of the following configurations:

| Configuration | Output |
|---|---|
| `Client Release\|x64` | Client DLL with save widget UI |
| `Server Release\|x64` | Server DLL (widget calls are no-ops) |

The output DLL will be placed in `build\<config>\Plugins\`.

---

## Disclaimer

Use at your own risk. The authors are not responsible for any damage caused by using this software.
