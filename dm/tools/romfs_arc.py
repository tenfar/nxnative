#!/usr/bin/env python3
"""Pack a RomFS tree into a ROMFSARC archive (one index file + N shard files).

Why: Tears of the Kingdom's RomFS is 304,000 files. Copying that into an App's Documents takes
hours, and a 16 GB IPA with that many entries fails to install on iOS (PackageExtractionFailed).
An archive is ~120 files instead, so one copy moves it.

Shard layout: files are grouped by their top-level directory (so one shard holds one kind of
content), then split so that no shard passes --shard-mib. Files are written in path order within
a shard; a shard never mixes content from different top-level directories unless that directory
alone exceeds the limit.

Usage:
  romfs_arc.py pack <romfs dir> <output base> [--shard-mib 200]
  romfs_arc.py list <base> [path]          # debug: list a directory
  romfs_arc.py verify <base> <romfs dir>   # compare every file's bytes against the source tree

Format: see dm/host/romfs_arc.h.
"""
import argparse
import collections
import os
import struct
import sys

MAGIC = b"ROMFSARC"
VERSION = 1
HDR = 64
FILEREC = 32
DIRREC = 16
CHILD_DIR = 0x80000000
MAX_SHARDS = 256   # must match romfs_arc.c (one fd slot per shard)


def fnv1a(data, h=0xCBF29CE484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def walk(root):
    """Every file and directory under root, as (relpath, size) / relpath. Relpaths use '/' and have
    no leading slash; the root itself is ''. Symlinks are followed (RomFS has none in the games
    seen so far); anything that is neither a file nor a directory is an error."""
    files, dirs = [], [""]
    for dp, dn, fn in os.walk(root):
        rel_dir = os.path.relpath(dp, root).replace(os.sep, "/")
        if rel_dir == ".":
            rel_dir = ""
        dn.sort()
        for d in dn:
            dirs.append(f"{rel_dir}/{d}" if rel_dir else d)
        for f in sorted(fn):
            p = os.path.join(dp, f)
            try:
                st = os.stat(p)
            except OSError as e:
                sys.exit(f"[RED] cannot stat {p}: {e}")
            files.append((f"{rel_dir}/{f}" if rel_dir else f, st.st_size))
    files.sort(key=lambda x: x[0])
    dirs.sort()
    return files, dirs


def plan_shards(files, shard_bytes):
    """Assign every file to a shard. Files are grouped by their top-level directory so one shard
    holds one kind of content, and each group is cut greedily into shards of at most shard_bytes.
    A single file larger than shard_bytes gets a shard of its own (the cap is a packing target, not
    a hard limit), so a shard can exceed it when it holds such a file alone."""
    by_top = collections.OrderedDict()
    for rel, size in files:
        parts = rel.split("/", 1)
        top = parts[0] if len(parts) > 1 else ""   # a file at the root has no group of its own
        by_top.setdefault(top, []).append((rel, size))
    # Big groups first, so the shard count stays close to size total / shard_bytes
    order = sorted(by_top.items(), key=lambda kv: -sum(s for _, s in kv[1]))
    assign = {}
    shard = 0                    # the shard currently being filled
    cur = 0                      # bytes placed in it
    used_any = False             # whether the current shard holds anything
    for _top, items in order:
        for rel, size in items:
            # Start a new shard when the next file would overflow this one. A file that is itself
            # larger than the cap still goes into a fresh shard, alone.
            if used_any and cur + size > shard_bytes:
                shard += 1
                cur = 0
                used_any = False
            assign[rel] = (shard, cur)
            cur += size
            used_any = True
        # A group boundary ends the current shard, so one shard never mixes two top-level directories
        if used_any:
            shard += 1
            cur = 0
            used_any = False
    return assign, shard


def pack(romfs, base, shard_mib):
    shard_bytes = shard_mib << 20
    files, dirs = walk(romfs)
    total = sum(s for _, s in files)
    print(f"pack: {len(files):,} files, {len(dirs):,} directories, {total / 2**30:.2f} GiB", flush=True)
    assign, nshard = plan_shards(files, shard_bytes)
    # The reader keeps one fd slot per shard (MAX_SHARDS in romfs_arc.h) and rejects an index above
    # it. Fail here instead of producing an archive the app cannot open, and say how to fix it.
    if nshard > MAX_SHARDS:
        # Minimum shard size that fits the total into MAX_SHARDS, rounded up to a whole MiB
        need_mib = max(1, (total + MAX_SHARDS - 1) // MAX_SHARDS // (1 << 20) + 1)
        sys.exit(f"[RED] {nshard} shards needed, but the reader supports {MAX_SHARDS}. "
                 f"Raise --shard-mib to at least {need_mib}.")
    print(f"pack: {nshard} shards of at most {shard_mib} MiB", flush=True)

    # Shard files: written in path order within each shard
    per_shard = collections.defaultdict(list)
    for rel, size in files:
        per_shard[assign[rel][0]].append((rel, size))
    os.makedirs(os.path.dirname(base) or ".", exist_ok=True)
    written = 0
    for i in range(nshard):
        path = f"{base}.{i:02d}"
        with open(path, "wb") as out:
            for rel, size in sorted(per_shard[i]):
                with open(os.path.join(romfs, rel), "rb") as f:
                    left = size
                    while left:
                        buf = f.read(min(left, 1 << 22))
                        if not buf:
                            sys.exit(f"[RED] {rel}: file shrank while packing")
                        out.write(buf)
                        left -= len(buf)
                written += size
        sz = os.path.getsize(path)
        # Sub-MiB shards are real (a group of tiny files); show KiB so they do not read as empty
        shown = f"{sz / 2**20:.0f} MiB" if sz >= (1 << 20) else f"{sz / 1024:.0f} KiB"
        print(f"\r  shard {i + 1}/{nshard}: {shown}", end="", flush=True)
    print(f"\n  wrote {written / 2**30:.2f} GiB in {nshard} shards", flush=True)

    # Name pool
    pool = bytearray()
    name_off = {}

    def intern(s):
        if s not in name_off:
            name_off[s] = len(pool)
            pool.extend(s.encode("utf-8") + b"\0")
        return name_off[s]

    for rel, _ in files:
        intern(rel)
    for d in dirs:
        intern(d)

    # File table (sorted by path, same order as `files`)
    file_idx = {}
    ftab = bytearray()
    for n, (rel, size) in enumerate(files):
        file_idx[rel] = n
        shard, off = assign[rel]
        ftab += struct.pack("<IIQQII", intern(rel), shard, off, size, 0o644, 0)

    # Directory table + child list. A directory's children are its immediate files and directories,
    # sorted by name, contiguous in the child list.
    dir_idx = {d: n for n, d in enumerate(dirs)}
    children_of = collections.defaultdict(list)
    for rel, _ in files:
        parent, _, name = rel.rpartition("/")
        children_of[parent].append((name, file_idx[rel]))
    for d in dirs:
        for sub in dirs:
            if sub and sub.rpartition("/")[0] == d and sub != d:
                # Only the leaf name goes in the child list; the directory's own path is in its record
                children_of[d].append((sub.rpartition("/")[2], dir_idx[sub] | CHILD_DIR))

    dtab = bytearray()
    child_list = []
    for d in dirs:
        kids = sorted(children_of[d])
        first = len(child_list)
        child_list.extend(tag for _, tag in kids)
        dtab += struct.pack("<IIII", intern(d), first, len(kids), 0)

    tree = bytearray()
    for rel, size in files:
        tree += f"{rel}\0{size}\0".encode()
    tree_hash = fnv1a(tree)

    idx_path = f"{base}.idx"
    with open(idx_path, "wb") as out:
        out.write(struct.pack("<8sIIQQQQQQ", MAGIC, VERSION, nshard, 0, len(files), len(dirs),
                              len(pool), 0, tree_hash))
        out.write(pool)
        out.write(ftab)
        out.write(dtab)
        out.write(struct.pack(f"<{len(child_list)}I", *child_list) if child_list else b"")
    print(f"pack: index {os.path.getsize(idx_path) / 2**20:.1f} MiB ({len(pool) / 2**20:.1f} MiB names, "
          f"{len(files):,} files, {len(dirs):,} directories), tree hash {tree_hash:016x}", flush=True)
    print(f"pack: total {nshard + 1} files", flush=True)


def load(base):
    with open(f"{base}.idx", "rb") as f:
        b = f.read()
    magic, ver, nshard, _pad, nfiles, ndirs, pool_sz, _r, tree_hash = struct.unpack_from("<8sIIQQQQQQ", b, 0)
    if magic != MAGIC or ver != VERSION:
        sys.exit("[RED] not a ROMFSARC index")
    pool = b[HDR:HDR + pool_sz]
    off = HDR + pool_sz
    files = []
    for i in range(nfiles):
        name_off, shard, foff, size, mode, _ = struct.unpack_from("<IIQQII", b, off + i * FILEREC)
        name = pool[name_off:pool.index(b"\0", name_off)].decode()
        files.append((name, shard, foff, size))
    off += nfiles * FILEREC
    dirs = []
    for i in range(ndirs):
        name_off, first, count, _ = struct.unpack_from("<IIII", b, off + i * DIRREC)
        name = pool[name_off:pool.index(b"\0", name_off)].decode()
        dirs.append((name, first, count))
    off += ndirs * DIRREC
    kids = struct.unpack_from(f"<{len(b) - off >> 2}I", b, off) if len(b) > off else ()
    return dict(nshard=nshard, files=files, dirs=dirs, kids=kids, tree_hash=tree_hash)


def cmd_list(base, path):
    a = load(base)
    target = path.strip("/")
    d = next((x for x in a["dirs"] if x[0] == target), None)
    if not d:
        sys.exit(f"[RED] no such directory: {target or '/'}")
    _, first, count = d
    print(f"{target or '/'}: {count} entries")
    for k in a["kids"][first:first + count]:
        if k & CHILD_DIR:
            print(f"  d  {a['dirs'][k & ~CHILD_DIR][0].rpartition('/')[2]}/")
        else:
            name, shard, off, size = a["files"][k]
            print(f"  f  {name.rpartition('/')[2]:50} {size:12,}  shard {shard:3} off {off:,}")


def cmd_verify(base, romfs):
    a = load(base)
    print(f"verify: {len(a['files']):,} files")
    bad = 0
    for n, (rel, shard, off, size) in enumerate(a["files"]):
        src = os.path.join(romfs, rel)
        try:
            with open(src, "rb") as f:
                want = f.read()
        except OSError as e:
            print(f"[RED] missing source {rel}: {e}")
            bad += 1
            continue
        if len(want) != size:
            print(f"[RED] size mismatch {rel}: source {len(want)} archive {size}")
            bad += 1
            continue
        with open(f"{base}.{shard:02d}", "rb") as f:
            f.seek(off)
            got = f.read(size)
        if got != want:
            print(f"[RED] content mismatch {rel}")
            bad += 1
        if (n + 1) % 20000 == 0:
            print(f"  {n + 1:,}/{len(a['files']):,}", flush=True)
    print(f"verify: {'OK' if not bad else f'{bad} problems'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="Pack a RomFS tree into a ROMFSARC archive")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("romfs")
    p.add_argument("base")
    p.add_argument("--shard-mib", type=int, default=200)
    l = sub.add_parser("list")
    l.add_argument("base")
    l.add_argument("path", nargs="?", default="")
    v = sub.add_parser("verify")
    v.add_argument("base")
    v.add_argument("romfs")
    a = ap.parse_args()
    if a.cmd == "pack":
        pack(a.romfs, a.base, a.shard_mib)
    elif a.cmd == "list":
        cmd_list(a.base, a.path)
    else:
        sys.exit(cmd_verify(a.base, a.romfs))


if __name__ == "__main__":
    main()
