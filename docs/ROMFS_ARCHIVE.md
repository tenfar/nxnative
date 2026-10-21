# RomFS archive (ROMFSARC)

A whole RomFS tree packed into one index file plus ~100 shard files, so a game whose RomFS is
hundreds of thousands of files can be copied to a phone as one folder instead of a directory tree.

## Why

Tears of the Kingdom's RomFS is 304,243 files / 15.9 GiB. Two problems follow:

- **The IPA does not install.** A 16.6 GB IPA with 372,660 entries fails on iOS with
  `PackageExtractionFailed (Could not extract archive)`. The install daemon's extractor gives up:
  one directory holds 75,941 files and the ZIP64 central directory starts at 17.8 GB.
- **Copying the tree is slow.** The Files app and Finder both do one round trip per file; 304,243
  small files take hours and often break partway.

Packing the tree into 127 files makes the copy one operation of ~16 GB and lets the IPA stay small.

## Using it

### 1. Pack the game files

```bash
python3 dm/tools/romfs_arc.py pack <RomFS dir> <output base> --shard-mib 200
```

`<RomFS dir>` is the directory `nx_extract` wrote (the one holding `Pack/`, `Banc/`, `Sound/`, …).
`<output base>` is a path prefix: the tool writes `<base>.idx` and `<base>.00`, `<base>.01`, ….
Files are grouped by top-level directory and cut so no shard passes `--shard-mib`, so one shard
holds one kind of content and no shard is unreasonably large.

For TotK at 200 MiB per shard: 126 shards + 1 index = **127 files**.

### 2. Copy it to the phone

Put the whole set in the app's `Documents/romfs/`, keeping the base name:

```
Documents/romfs/romfs.idx
Documents/romfs/romfs.00
...
Documents/romfs/romfs.125
```

The app looks for `romfs.idx` first (`dm/ios/Sources/main.m`, `romfs_locate`); a plain directory
tree is still accepted when there is no index.

### 3. Build the app without the RomFS

```bash
DM_NO_ROMFS=1 GAME=<title id> W=... PATCHED=... ROMFS=... MSL_IOS=... \
  dm/ios/tools/build_ipa.sh out.ipa
```

The package then holds only the app, the shader libraries and the tables (~750 MB for TotK), and
the game files come from `Documents/romfs/` at run time.

## Checking an archive

```bash
python3 dm/tools/romfs_arc.py list   <base> [path]     # list one directory
python3 dm/tools/romfs_arc.py verify <base> <RomFS>    # compare every file's bytes with the source
```

`verify` reads all 304,243 files of TotK's archive in about a minute and prints `verify: OK`.

## Format

See `dm/host/romfs_arc.h` for the byte layout. In short: a 64-byte header, a NUL-separated name
pool, a file table sorted by path (32 bytes each), a directory table (16 bytes each) and a child
list. Path lookup is a binary search; a directory listing walks one contiguous child range.

The reader (`dm/host/romfs_arc.c`) maps the index with `mmap` and opens shards lazily, keeping up
to 256 descriptors. Reads go straight to `pread` on the shard, so the page cache still works.

## How the fs layer uses it

`hle_fs.c` keeps its normal path handling. When an archive is open, `host_path_ex` returns a
synthetic path (`@romfsarc:<guest path>`) for entries on the archive mount instead of a host file
path; `OpenFile`, `GetEntryType`, `OpenDirectory` and the directory calls recognise that prefix and
route to `romfs_arc.c`. File handles for archive entries are marked with bit 63 so they never
collide with real fds, and `ReadFile` clamps to the file size (a shard is a flat container, so a
read past the end would otherwise return the next file's bytes).

The mount is recognised by its **directory**, not its name: titles differ (`rom` for most games,
`content` for Tears of the Kingdom).

## Limitations

- **Read-only.** Writes go to the save/temp mounts as before; the archive is never written.
- **Case-sensitive lookup.** The archive stores the names exactly as the source tree had them, so
  the case-insensitive alias machinery the host uses for APFS directories is bypassed.
- **Symlinks are followed at pack time.** No supported game's RomFS has any.
- **The index is held in memory.** 28 MB for TotK (17.9 MB of it names); the shards are not.
- **No dedup or compression.** Shards are the source bytes verbatim, so `verify` is a plain
  comparison and a repack is deterministic.
