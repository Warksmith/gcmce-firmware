<p align="center">
    <img src="doc/FlipperMCE.png" alt="FlipperMCE Logo" width="20%">
</p>

# FlipperMCE Firmware

FlipperMCE is a *MemoryCard Emulator* that is heavily based on sd2psx by developer @xyzz (see [here](https://github.com/sd2psx)).

## Branch organization

`integration` combines the fork's features for builds and testing. Each feature branch is scoped for a separate upstream PR against `FlipperMCE/firmware:main`:

| Branch | Scope | Prerequisite |
| --- | --- | --- |
| `card-data-integrity` | Erase bounds, dirty flushes and card exit fixes | Upstream main |
| `mass-storage` | USB microSD access, safe-disconnect display and read-ahead | Upstream main |
| `folder-backend` | GCI folder cards, recovery, tests and global backend setting | `card-data-integrity` |
| `game2folder-wildcards-default` | GameID prefix mappings and optional fallback | Upstream main |
| `startup-card` | Configurable card at boot, with CardRestore precedence | Upstream main |

Develop on the owning feature branch, then merge into `integration`. Submit the integrity fixes before folder cards. Branches under `archive/` preserve the former stacked history and are not PR candidates.

## New Features in this repo

## Folder-Backed Memory Cards (experimental)

A card can keep its saves as individual Dolphin-compatible `.gci` files instead of one `.raw` image.

**Per memory card:**  Add `StorageBackend=FOLDER` to the `[Settings]` section of its `CardX.ini`, and its saves live in `CardX-1.folder` next to where the `.raw` would be.

`F:\MemoryCards\GC\Card123\Card123.ini`
```
[Settings]
StorageBackend=FOLDER

```

**Global:** The same section in `.flippermce/settings.ini` makes it the default for every card that does not set its own. (Existing `.raw` cards are not touched or converted.) Works for cards created automatically via GameID.

`F:\.flippermce\settings.ini`
```
[Settings]
StorageBackend=FOLDER

```

 See [doc/folder-memory-cards.md](doc/folder-memory-cards.md).


## Load Card on Startup

With `Card Restore` off, choose a fixed startup card in `.flippermce/settings.ini`:

```ini
[GC]
CardRestore=OFF
StartupCard=MyStartupCard
```

This opens channel 1 of `MemoryCards/GC/MyStartupCard` at power-up. You can also use a
numbered card such as `StartupCard=Card12`, or a full GameID folder name.
Missing cards are created through the normal card creation process, using the
global/per-card storage backend and size settings. Existing cards are loaded normally.

`CardRestore=ON` takes priority over `StartupCard`. An empty or omitted
`StartupCard` keeps the original Card1 behavior. Use a single folder name of up
to 31 characters, without path separators or filename-invalid characters;
invalid values fall back to Card1. This setting is read from the SD card and
preserved when the device rewrites settings; it does not reset flash settings.
GameID switching can still select another card after startup. `DefaultCard` in
`Game2Folder.ini` remains a separate fallback for unmatched GameIDs.


## USB storage mode

Hold Button 1 while connecting USB to use the microSD card as a removable drive, with no card reader. Eject it on the computer before unplugging. Button 2 still starts the UF2 firmware update mode. (If both buttons are held, firmware update mode takes priority). See [doc/usb-storage.md](doc/usb-storage.md).

#### ==End of features specific to this fork==



## Features

It has the following base feature set:

- **Card Restore**
- **Game ID**
- **Multiple CardSizes**
- **Game2Folder mapping**
- **Settings File**
- **Per Card Configs**


### Card Restore

If active, FlipperMCE will always boot to the last used card.

```ini
[GC]
CardRestore=OFF
```

### Game ID

Many loaders implement a special protocol, that allows the cube to transmit the Game ID of the currently started game to an *MCE*. FlipperMCE supports this protocol (see *doc/mcp*) and will load or create a card for each trasmitted Game ID if configured so.

### Mulitple CardSizes

*FlipperMCE* support Card Sizes from 4 - 64 MBits.


## Game2Folder mapping

There are some games, that share save data for multiple game ids. For these cases, a custom game to folder mapping can be created.

If a game with a mapped id is loaded, instead of using the game id based folder, the mapped folder is used for storing the card.

The mapping needs to be defined in ```.flippermce/Game2Folder.ini``` in the following way:

```ini
[GC]
DL-DOL-GT4P-EUR=FolderName
```

Use the full GameID reported by the firmware (`DL-DOL-<four-character ID>-<region>`).
Trailing `*` wildcards group IDs by prefix; repeated trailing stars mean the same
thing as one star. For example:

```ini
[GC]
DL-DOL-GT4P-EUR=ExactCard
DL-DOL-GT4*=SharedSeries
DL-DOL-*=AllGames
UseDefaultCard=ON
DefaultCard=Shared
```

Exact IDs take priority over wildcards, then the longest matching prefix wins,
regardless of line order. Equal-specificity matches use the last entry. Matching
is case-sensitive. Only trailing stars are supported, not `?` or embedded stars.
A bare `*` matches every nonempty ID.

`UseDefaultCard=ON` enables `DefaultCard` only when no mapping matches. It is off
when omitted; `OFF` or any value other than `ON` disables it (`ON` is
case-insensitive). Remove `DL-DOL-*=AllGames` from this example to let unmatched
GameCube IDs use `Shared`. Without fallback, unmatched games keep the normal
per-GameID card behavior. An empty mapping explicitly selects that normal
behavior, even with fallback enabled.

Targets are card folder names under `MemoryCards/GC`, limited to 31 characters
(longer names are truncated). For example, `DefaultCard=Shared` selects
`MemoryCards/GC/Shared` and uses its normal card/channel naming and settings.
Both raw and folder-backed cards are supported. GameID must be enabled in
`settings.ini`; this does not change boot-time Card Restore behavior.
The lookup runs during card selection, with no extra work on card reads or writes.

*Note: Be aware: Long folder names may not be displayed correctly and may result in stuttering of MMCE games due to scrolling.*
*Note 2: Make sure there is an empty line at the end of the ini file.*


### Settings File

*FlipperMCE* generates a settings file (`.flippermce/settings.ini`) that allows you to edit some settings through your computer. This is useful when using one SD card with multiple *FlipperMCE* devices.

A settings file has the following format:

```ini
[General]
FlippedScreen=OFF
[GC]
CardRestore=ON
StartupCard=
GameID=ON
CardSize=64
```

Possible values are:

| Setting       | Values                                |
|---------------|---------------------------------------|
| CardRestore   | `OFF`, `ON`                           |
| StartupCard   | Card folder name; empty uses Card1 when CardRestore is OFF |
| GameID        | `OFF`, `ON`                           |
| CardSize      | `4`, `8`, `16`, `32`, `64`            |
| FlippedScreen | `ON`, `OFF`                           |

*Note: Make sure there is an empty line at the end of the ini file.*

### Per Card Configs

There are some configuration values that can be modified on a per card base within a config file named  `CardX.ini` in a card folder, where `X` is the card index.

*Note: Make sure there is an empty line at the end of the ini file.*

```ini
[ChannelName]
1=Channel 1 Name
2=Channel 2 Name
3=Channel 3 Name
4=Channel 4 Name
5=Channel 5 Name
6=Channel 6 Name
7=Channel 7 Name
8=Channel 8 Name
[Settings]
MaxChannels=8
CardSize=8
```

### Card splashes

Add a splash image that the device shows in the card browser. Use the included splash generator (`misc/splashgen.html`) to convert a source image to the device `.bin` format, then place the generated file in the card folder on your SD card.

Naming and behavior
- Folder-level splash (default for the folder):
    - Path: `MemoryCards/GC/<card_folder>/<card_folder>.bin`
    - Example: `MemoryCards/GC/SuperGame/SuperGame.bin` — used when no channel-specific image exists.
- Channel-specific splash (overrides folder-level for that channel):
    - Path: `MemoryCards/GC/<card_folder>/<card_folder>-<channel_number>.bin`
    - Example: `MemoryCards/GC/SuperGame/SuperGame-1.bin` — shown only for channel 1 of that card folder.
- Fallback rules:
    - If a channel-specific file exists it is used.
    - Otherwise the folder-level `<card_folder>.bin` is used.
    - If neither exists, no splash is shown.

Practical notes
- `<channel_number>` matches the on-device channel index (1..N).
- Filenames must match exactly; FAT SD cards are usually case-insensitive but keep names consistent.
- Use `misc/splashgen.html` to produce correctly-sized and packed `.bin` files for the OLED.
- Keep names short — very long filenames may cause display or performance issues.
- Store splash `.bin` files alongside that card's `CardX.ini` and save data in the same folder.


## Known issues

### wii will not recognize the card as memory card

Within the wii System Menu, the card may occasionally not be recognized. If this happens please try the following things:

- Close and re-open Gamecube Cardbrowser
- Switch the mounted card using the buttons on FlipperMCE
- Re-Plug FlipperMCE

## Special Thanks to...

- **Vapor, rippenbiest, Mancloud**: for beta testing ❤️
- **@gameBitfunx**: for PCB design, testing and support ❤️
- **@xyz**: for sd2psx ❤️
- **sd2psXtd Team**: (you know who you are 😉 )
- **8BitMods Team**: for helping out with card formatting and providing lots of other useful information for things like unlock ❤️
- **@extrems**: For insights into EXI communications and libOGC2 SDK

