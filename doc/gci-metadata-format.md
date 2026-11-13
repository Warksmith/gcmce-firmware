# Folder card file formats

The formats of a folder card's files. All of these are private to the firmware except the
`.gci` files. A change to any layout must change its version or magic.

```text
Card1-1.folder/
├── *.gci                        one save each
└── .flippermeta/
    ├── card.dat                 identity
    ├── saves.dat                what was last committed
    ├── journal.dat              only during a transaction
    ├── import-errors.txt        only after a refused mount
    ├── stage/<n>.bin            only during a transaction
    └── trash/<seq>/*.gci        saves removed by a format
```

Only `card.dat` holds anything that cannot be rebuilt from the `.gci` files. `stage` and
`journal.dat` are empty or absent unless a transaction is running.

## .gci files

A `.gci` is a directory entry followed by the save's blocks, in order:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 64 | The save's directory entry, big-endian as on the card |
| 64 | 8192 × blocks | The data blocks, in chain order |

This is Dolphin's layout, and the file must be exactly `64 + 8192 × block count` bytes.

A save is identified by game code, maker code and file name up to the first NUL, compared
as bytes. Two files with one identity are refused. Case counts: `Save` and `SAVE` are two
saves on the console, and only their host names have to differ.

The first block field of the entry is the only one that has no meaning off the card. It is
written as the card had it, and ignored and rewritten at import.

### Host names

A save with no host name yet gets `MM-GGGG-filename.gci`, from the maker code, game code and
file name. Control characters, bytes from 0x7F up, `" * / : < > ? \ | %` and a space at the
start of the name are written as `%XX`. A name that would exceed 127 characters is cut before
the extension, so that the result is still a `.gci`.

A name that collides with another save, or with one leaving in the same transaction,
ignoring case as FAT does, gets `~XXXXXXXX` before the extension. Names are never reused
within a transaction, which is what makes a repeated trash operation safe.

An existing host name is kept for as long as the save exists.

## card.dat

Exactly 512 bytes: the card's header block, as it is at offset 0 of a card image. It holds
the serial, format time, size and encoding, and its checksum must be valid.

If it is missing, a header is made like a new `.raw` card's. If it is damaged, the mount is
refused, because replacing it would silently change the card's identity. If the ini's
`CardSize` differs, the size field and the checksum are updated and nothing else.

## saves.dat

All values are little-endian. It is rewritten through the journal, never in place.

Head, 20 bytes:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 8 | `GCFSAVE1` |
| 8 | 4 | Version, 1 |
| 12 | 4 | Record size, 200 |
| 16 | 4 | Sequence number of the transaction that wrote it |

Then one record per save:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 64 | Directory entry |
| 64 | 4 | CRC-32 of the save's data blocks |
| 68 | 4 | Directory slot |
| 72 | 128 | Host name, NUL-terminated and padded with zeroes |

Trailer, 16 bytes:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | Record count |
| 4 | 4 | CRC-32 of everything before this field |
| 8 | 8 | `GCFSEND1` |

A file that fails any check is treated as missing: every save then gets a slot from scratch
and the file is rebuilt. Only slot order is lost.

## journal.dat

Little-endian. The file takes effect when its trailer exists, and the size alone tells:
`16 + 176 × n + 16` bytes is a committed journal, any other size is not.

Head, 16 bytes: `GCFJRN01`, the sequence number, and 4 bytes of zero.

Then one operation per step, 176 bytes:

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 4 | Type: 1 replace, 2 remove, 3 trash |
| 4 | 4 | Stage file number, for replace |
| 8 | 4 | Stage file size, for replace |
| 12 | 4 | CRC-32 of the whole stage file, for replace |
| 16 | 156 | Path relative to the card folder, NUL-terminated and padded with zeroes |
| 172 | 4 | CRC-32 of the 172 bytes before it |

Trailer, 16 bytes: the operation count, CRC-32 of everything before that field including the
count, and `GCFJEND1`.

| Operation | Effect |
| --- | --- |
| Replace | Copy the stage file over the path, checking the CRC while copying |
| Remove | Delete the path if it exists |
| Trash | Copy the path to `trash/<seq>/`, then delete it. Done if it is already gone |

`<seq>` is the sequence number as 8 hexadecimal digits. Every operation can be repeated, as
recovery may run any of them twice.

## Stage files

`stage/<n>.bin`, where `n` is the directory slot of a save (0 to 126), 200 for `saves.dat` or
201 for `card.dat`. A save's stage file is its complete future `.gci`.

## CRCs

CRC-32 with the usual reflected polynomial 0xEDB88320, as `gf_crc32()` in `gc_gci.c`.

## import-errors.txt

Plain text, one line per file, written when a mount is refused and removed by the next
successful one.
