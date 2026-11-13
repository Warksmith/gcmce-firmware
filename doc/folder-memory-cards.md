# Folder memory cards

A folder card keeps every save as its own `.gci` file, the format Dolphin uses, instead of
one `.raw` image of a whole card. Saves can be added, removed, backed up and copied to and
from Dolphin one at a time.

The console sees an ordinary memory card. At mount the firmware builds a complete card in
PSRAM from the `.gci` files, and it writes changes back to individual files afterwards.

`.raw` cards stay the default and work exactly as before. A card only becomes a folder card
when its ini says so.

## Turning it on

Add one line to the `[Settings]` section of the card's `CardX.ini`:

```ini
[Settings]
StorageBackend=FOLDER
CardSize=16
```

The value is `FOLDER` or `RAW`, in capitals. Anything else, or no line at all, means `RAW`.
A folder existing on the SD card does not turn the feature on.

Each channel of a card has its own folder, next to where its `.raw` would be:

```text
MemoryCards/GC/Card1/
├── Card1.ini
├── Card1-1.raw                 not touched by a folder card
└── Card1-1.folder/
    ├── 01-GAFE-MarioKart.gci   the saves
    ├── 01-GZLE-Zelda.gci
    └── .flippermeta/           the firmware's own files
```

The folder is created at the first mount. Nothing is converted, and a `.raw` file is never
read, changed or removed. To move saves out of a `.raw` card, export them as `.gci` with
Dolphin or another memory card manager and copy them in.

Game ID cards and Game2Folder mappings work the same way: put `StorageBackend=FOLDER` in the
ini of that card's folder.

## Card sizes

4, 8, 16, 32 and 64 Mbit work, as for `.raw` cards. These are the blocks a save can use:

| Size | Blocks |
| --- | --- |
| 4 Mbit | 59 |
| 8 Mbit | 123 |
| 16 Mbit | 251 |
| 32 Mbit | 507 |
| 64 Mbit | 1019 |

The size comes from `CardSize` in the ini if there is one, then from the card's own
identity once it has one, then from the default in the settings. A card holds at most 127
saves whatever its size.

## Adding saves from Dolphin or a computer

1. Start [USB storage mode](usb-storage.md), or put the SD card in a reader.
2. Copy `.gci` files into the card's `.folder` directory. Dolphin keeps them in
   `User/GC/<region>/Card A`. Any file name ending in `.gci` will do.
3. Eject properly and boot.

The firmware loads every `.gci` in the folder at the next mount. Files other than `.gci`,
and everything in subfolders, are left alone.

The saves you get back from the card are normal `.gci` files too. Copy them to Dolphin's
folder, or to another folder card, the same way. Only `.gci` works: `.gcs` and `.sav` need
converting first.

A save written by a game is called `MM-GGGG-filename.gci`, from its maker code, game code and
file name, the way Dolphin names it. Characters a file name cannot hold are written as `%XX`.
A file you named yourself keeps that name when the game saves over it.

## When a card will not load

If any `.gci` cannot be used, the card stays hidden from the console. Nothing is skipped and
no file is changed, so a save is never missing without you knowing. The display shows the
reason, and `.flippermeta/import-errors.txt` lists each file.

| Display | Meaning | What to do |
| --- | --- | --- |
| `Bad GCI file` | Not a valid `.gci`, or its size does not match its header | Replace or remove it |
| `Duplicate save` | Two files hold the same save | Remove one |
| `Too big for card` | The saves need more blocks than the card has | Raise `CardSize`, or remove some |
| `Too many saves` | More than 127 saves | Remove some |
| `Bad GCI name` | The name is too long (over 127 characters) or not plain ASCII | Rename it |
| `Bad card.dat` | The card's identity is damaged | See below |
| `Recovery required` | A save was interrupted and part of what it needs is gone | Keep `.flippermeta` and see [recovery](folder-sync-recovery.md) |
| `SD error` | The SD card could not be read or written | Check it. Reboot to try again |

Once the problem is fixed the next mount succeeds and the report goes away.

If `card.dat` is damaged, deleting it gives the card a new identity. The saves are not
affected, but a game that ties its save to the card may notice: see
[Protected saves](#protected-saves).

## While the console is running

The console reads and writes PSRAM. After it has been quiet for about 1.5 seconds, the
firmware saves what changed in the background. The display shows what is going on:

| Display | Meaning |
| --- | --- |
| `Loading saves...` | The card is being built, with the percentage |
| `Unsaved changes` | Waiting for the console to go quiet |
| `Saving card...` | Writing to the SD card |
| `Save failed!` | Writing failed. It is retried every few seconds |
| `Card data invalid` | The card in PSRAM is not a valid filesystem, so it is not being saved |

**Do not cut the power while the display shows anything other than the card name.** Until
`Saving card...` is gone, a save the game reported as finished exists only in PSRAM. A power
cut then loses that change, and leaves the previous version of the save intact. It can never
leave half a save: see [recovery](folder-sync-recovery.md).

Switching to another card saves everything first. If that fails, the switch is refused and
the card stays mounted, because the changes exist nowhere else. A reboot gives up on them.

## Deleted saves

Deleting one save on the console deletes its `.gci`.

Formatting the card, or deleting the last save on it, moves the files to
`.flippermeta/trash/<number>/` instead. Those are the cases where a mistake costs the most,
and a console that lost track of a card can look just like them. Trash is never emptied
automatically, so clear it yourself when you are sure.

## Protected saves

A few games tie a save to the card it was made on, usually through the card's serial number.
Those saves can refuse to load on a different card, and F-Zero GX and Phantasy Star Online
are the known examples.

The firmware does not rewrite any save. Dolphin patches these saves when it builds a card;
this firmware deliberately does not, so that a `.gci` is the same bytes on the way out as on
the way in. What it does do:

- A card keeps one identity, stored in `.flippermeta/card.dat`, for as long as that file
  exists. Saves made on it keep working on it.
- A new folder card has the same fixed identity as a new `.raw` card from this firmware.
- To use a protected save from another card, give the folder card that card's identity:
  make `.flippermeta/card.dat` the first 512 bytes of that card's image, before the first
  mount or with the console off. This has not been tested on a console.

## Backups

The `.gci` files are the backup: copy the folder. Copy it with `.flippermeta` to keep the
card's identity and file order as well. `.flippermeta` can be left out, and the card will
rebuild everything except the identity.

## Known limitations

- A card loads only at mount. Files changed on the SD card while it is mounted are not
  noticed, which cannot happen in practice since storage mode runs on its own.
- The first mount of a large card takes longer than a `.raw` card, since every save is read
  and checked.
- If a game changes data and the power goes during a pass, the card can come back with the
  previous version of that save. It is complete, but it is older than the game believes.
- A save is written when the console goes quiet, not when the game finishes it. A game that
  writes a block, pauses for several seconds and writes another can have the pause captured
  as a save. The next write corrects it.
- Save files keep their directory entry but not their place on the card, so a save can sit
  somewhere else after a remount. No known game depends on this, but it is a difference from
  a `.raw` card.
- Trash grows until it is cleared by hand.
