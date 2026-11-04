#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cp86img.py - build a bootable CardPuter86 floppy image from your own DOS disk.

CardPuter86 runs MS-DOS / PC-DOS inside an 8086 emulator, and the emulated
machine boots from a raw floppy image that lives on the ESP32-S3 internal flash
partition (or on the microSD card).  This tool takes *your own* DOS boot image
and turns it into a CardPuter86 system image: it drops the installer payload,
injects the bundled guest programs, writes a sane AUTOEXEC.BAT and then proves
every single byte it wrote by reading it back out of the finished image.

Nothing here distributes DOS - you must supply the DOS image yourself.

Design rules (each one exists because of a bug we hit for real):

  * Parse the BPB, never hard code 360K/1.44M geometry.  A tool that assumes a
    size silently corrupts every other size.
  * Reject names that are not valid 8.3 instead of truncating them.  An earlier
    version quietly truncated CPPSCREEN.EXE to CPPSCREE.EXE and DOS answered
    "Bad command or file name" - a truncated name is a wrong name, not a warning.
  * Always update every FAT copy.  A stale second FAT is the classic FAT12
    corruption that makes DOS report "General failure reading drive A".
  * Allocate by scanning the FAT for zero entries, not by walking the root
    directory.  Walking the root misses clusters that a deleted file still owns.
  * Free space is checked *before* the first byte is written, so a failed build
    never leaves a half-written image behind.
  * Read every added file back and compare digests.  "It looked fine" is not a
    build result.

Usage:
  python3 cp86img.py info    DISK.IMG
  python3 cp86img.py list    DISK.IMG
  python3 cp86img.py build   --base DOS.IMG --out CARD.IMG [options]
  python3 cp86img.py add     DISK.IMG DOSNAME LOCALFILE
  python3 cp86img.py del     DISK.IMG DOSNAME [DOSNAME...]
  python3 cp86img.py get     DISK.IMG DOSNAME OUTFILE
  python3 cp86img.py verify  DISK.IMG DOSNAME LOCALFILE

Run `python3 cp86img.py build --help` for the full option list.
"""

import argparse
import hashlib
import os
import struct
import sys
import time

VERSION = "1.0.0"

# Floppy geometries the emulator understands.
# Keep in sync with configure_image_geometry() in
# ESP32/CardPuter86/CardPuter86/cardputer_storage.cpp - a size outside this
# table still boots on a PC but falls back to a hard-disk geometry inside
# CardPuter86, and DOS then cannot read the disk at all.
SUPPORTED_GEOMETRY = {
    163840:  (1, 8,  40, "160K"),
    184320:  (1, 9,  40, "180K"),
    327680:  (2, 8,  40, "320K"),
    368640:  (2, 9,  40, "360K"),
    737280:  (2, 9,  80, "720K"),
    1228800: (2, 15, 80, "1.2M"),
    1474560: (2, 18, 80, "1.44M"),
    2949120: (2, 36, 80, "2.88M"),
}

# Files that only exist to install DOS from the retail floppy set.  They are
# useless on a running system and they are big: DOS 5.0 keeps its whole
# distribution as DOS50.ZIP (1.08 MB) on the single-disk version.
INSTALLER_PAYLOAD = [
    "SETUP.EXE", "SETUP.INI", "DOS50.ZIP", "PKUNZIP.EXE", "WINA20.386",
]

DIR_ENTRY_SIZE = 32
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
FREE = 0xE5
END_OF_DIR = 0x00

ALLOWED_83 = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$#@!%&'()-{}^~`")


class ImageError(Exception):
    """Anything that should stop the build with a readable message."""


# --------------------------------------------------------------------------
# 8.3 name helpers
# --------------------------------------------------------------------------
def split_83(name):
    """Validate a user supplied name and return (base, ext) padded to 8.3.

    Raises ImageError instead of truncating.  Every character in the result is
    upper case because FAT stores the on-disk name upper case for DOS files.
    """
    up = name.strip().upper()
    if not up:
        raise ImageError("empty file name")
    if "/" in up or "\\" in up:
        raise ImageError("%r: subdirectories are not supported, use a bare name" % name)
    if up.count(".") > 1:
        raise ImageError("%r: only one dot is allowed in an 8.3 name" % name)
    if "." in up:
        base, ext = up.split(".")
    else:
        base, ext = up, ""
    if len(base) < 1 or len(base) > 8:
        raise ImageError(
            "%r: name part must be 1-8 characters (this would be truncated to %r, "
            "and DOS cannot run a truncated name)" % (name, base[:8]))
    if len(ext) > 3:
        raise ImageError(
            "%r: extension must be 0-3 characters (this would be truncated to %r)"
            % (name, ext[:3]))
    for part, label in ((base, "name"), (ext, "extension")):
        for ch in part:
            if ch not in ALLOWED_83:
                raise ImageError(
                    "%r: character %r is not valid in a DOS %s" % (name, ch, label))
    return base.ljust(8).encode("ascii"), ext.ljust(3).encode("ascii")


def join_83(base_raw, ext_raw):
    base = base_raw.decode("latin-1").rstrip()
    ext = ext_raw.decode("latin-1").rstrip()
    return base + ("." + ext if ext else "")


def dos_datetime(fixed=True):
    """Return (time_word, date_word) for a directory entry.

    A fixed stamp keeps builds reproducible; --mtime now opts into the clock.
    """
    if fixed:
        y, mo, d, h, mi, s = 1991, 6, 15, 12, 0, 0
    else:
        t = time.localtime()
        y, mo, d, h, mi, s = t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec
    if y < 1980:
        y = 1980
    if y > 2107:
        y = 2107
    date_word = ((y - 1980) << 9) | (mo << 5) | d
    time_word = (h << 11) | (mi << 5) | (s // 2)
    return time_word, date_word


# --------------------------------------------------------------------------
# The image
# --------------------------------------------------------------------------
class Fat12Image(object):
    """A FAT12 floppy image addressed at byte level."""

    def __init__(self, path=None, raw=None, name="<mem>"):
        self.path = path
        self.name = name
        if raw is not None:
            self.raw = bytearray(raw)
        else:
            with open(path, "rb") as fh:
                self.raw = bytearray(fh.read())
        self._parse_bpb()

    # -- BPB ---------------------------------------------------------------
    def _parse_bpb(self):
        b = self.raw
        if len(b) < 512:
            raise ImageError("%s: file is smaller than one sector" % self.name)
        oem = bytes(b[3:11])
        (self.bps,) = struct.unpack_from("<H", b, 11)
        self.spc = b[13]
        (self.reserved,) = struct.unpack_from("<H", b, 14)
        self.nfats = b[16]
        (self.nroot,) = struct.unpack_from("<H", b, 17)
        (total16,) = struct.unpack_from("<H", b, 19)
        self.media = b[21]
        (self.spf,) = struct.unpack_from("<H", b, 22)
        (self.spt,) = struct.unpack_from("<H", b, 24)
        (self.heads,) = struct.unpack_from("<H", b, 26)
        (total32,) = struct.unpack_from("<I", b, 32)
        self.total_sectors = total16 or total32
        self.jump = bytes(b[0:3])
        try:
            self.oem = oem.decode("latin-1").rstrip()
        except Exception:
            self.oem = repr(oem)

        if self.bps not in (512, 1024, 2048, 4096):
            raise ImageError("%s: implausible bytes/sector %d - not a FAT image?"
                             % (self.name, self.bps))
        if self.spc == 0 or (self.spc & (self.spc - 1)) != 0:
            raise ImageError("%s: sectors/cluster must be a power of two, got %d"
                             % (self.name, self.spc))
        if self.nfats not in (1, 2):
            raise ImageError("%s: expected 1 or 2 FATs, got %d" % (self.name, self.nfats))
        if self.spf == 0:
            raise ImageError("%s: sectors/FAT is zero - the BPB is broken" % self.name)
        if self.total_sectors == 0:
            raise ImageError("%s: BPB declares zero sectors" % self.name)

        self.fat_off = self.reserved * self.bps
        self.root_off = (self.reserved + self.nfats * self.spf) * self.bps
        self.data_off = self.root_off + self.nroot * DIR_ENTRY_SIZE
        self.cluster_bytes = self.bps * self.spc
        data_sectors = self.total_sectors - (self.reserved + self.nfats * self.spf +
                                             self.root_sectors())
        if data_sectors <= 0:
            raise ImageError("%s: BPB leaves no data area" % self.name)
        self.data_clusters = data_sectors // self.spc
        self.max_cluster = self.data_clusters + 1
        if self.data_clusters >= 4085:
            raise ImageError(
                "%s: %d data clusters means this is FAT16, not FAT12; "
                "CardPuter86 needs FAT12 floppy images"
                % (self.name, self.data_clusters))

        expected = self.total_sectors * self.bps
        self.size_mismatch = None
        if expected != len(self.raw):
            self.size_mismatch = (expected, len(self.raw))

    def root_sectors(self):
        return (self.nroot * DIR_ENTRY_SIZE + self.bps - 1) // self.bps

    # -- FAT ---------------------------------------------------------------
    def _fat_byte_off(self, cl, copy=0):
        base = self.fat_off + copy * self.spf * self.bps
        return base + cl + (cl >> 1)

    def read_fat(self, cl):
        off = self._fat_byte_off(cl)
        if cl & 1:
            return ((self.raw[off] >> 4) | (self.raw[off + 1] << 4)) & 0xFFF
        return (self.raw[off] | ((self.raw[off + 1] & 0x0F) << 8)) & 0xFFF

    def write_fat(self, cl, value):
        for copy in range(self.nfats):
            off = self._fat_byte_off(cl, copy)
            if cl & 1:
                self.raw[off] = (self.raw[off] & 0x0F) | ((value & 0x0F) << 4)
                self.raw[off + 1] = (value >> 4) & 0xFF
            else:
                self.raw[off] = value & 0xFF
                self.raw[off + 1] = (self.raw[off + 1] & 0xF0) | ((value >> 8) & 0x0F)

    def fat_copies_differ(self):
        """Return the first cluster whose copies disagree, or None."""
        if self.nfats < 2:
            return None
        per_fat = self.spf * self.bps
        for cl in range(0, self.max_cluster + 1):
            first = self._fat_byte_off(cl, 0)
            for copy in range(1, self.nfats):
                if self.raw[first] != self.raw[first + copy * per_fat]:
                    return cl
        return None

    # -- clusters ----------------------------------------------------------
    def cluster_off(self, cl):
        return self.data_off + (cl - 2) * self.cluster_bytes

    def free_clusters(self):
        """Every cluster whose FAT entry is zero, in ascending order."""
        out = []
        for cl in range(2, self.max_cluster + 1):
            if self.read_fat(cl) == 0:
                out.append(cl)
        return out

    def chain_of(self, cl):
        chain, seen = [], set()
        while 2 <= cl <= self.max_cluster:
            if cl in seen:
                raise ImageError("%s: cluster loop at %d (the FAT is corrupt)"
                                 % (self.name, cl))
            seen.add(cl)
            chain.append(cl)
            cl = self.read_fat(cl)
        return chain

    # -- directory ---------------------------------------------------------
    def entries(self):
        """Yield (slot, offset, name, attr, cluster, size) for live entries."""
        for slot in range(self.nroot):
            off = self.root_off + slot * DIR_ENTRY_SIZE
            e = self.raw[off:off + DIR_ENTRY_SIZE]
            if len(e) < DIR_ENTRY_SIZE or e[0] in (FREE, END_OF_DIR):
                continue
            attr = e[11]
            (cl,) = struct.unpack_from("<H", e, 26)
            (size,) = struct.unpack_from("<I", e, 28)
            yield slot, off, join_83(e[0:8], e[8:11]), attr, cl, size

    def files(self):
        return [ent for ent in self.entries()
                if not ent[3] & (ATTR_DIRECTORY | ATTR_VOLUME_ID)]

    def audit(self):
        """Cross-check the FAT against the root directory.

        Returns (owned, leaked, crossed, bad_eof):
          owned   - clusters reachable from a real file
          leaked  - clusters the FAT marks in use but no file owns
          crossed - clusters owned by more than one file (cross-linked)
          bad_eof - files whose last cluster has no end-of-chain marker
        """
        owned = set()
        crossed = set()
        bad_eof = 0
        for slot, off, name, attr, cl, size in self.files():
            if size == 0:
                continue
            chain = self.chain_of(cl)
            for c in chain:
                if c in owned:
                    crossed.add(c)
                owned.add(c)
            if self.read_fat(chain[-1]) < 0xFF8:
                bad_eof += 1
        leaked = set()
        for cl in range(2, self.max_cluster + 1):
            if self.read_fat(cl) != 0 and cl not in owned:
                leaked.add(cl)
        return owned, leaked, crossed, bad_eof

    def find(self, dosname):
        base, ext = split_83(dosname)
        want = join_83(base, ext)
        for ent in self.entries():
            if ent[2] == want:
                return ent
        return None

    def free_slots(self):
        n = 0
        for slot in range(self.nroot):
            off = self.root_off + slot * DIR_ENTRY_SIZE
            if self.raw[off] in (FREE, END_OF_DIR):
                n += 1
        return n

    def _slot_for_new_entry(self):
        for slot in range(self.nroot):
            off = self.root_off + slot * DIR_ENTRY_SIZE
            if self.raw[off] == FREE:
                return off
        for slot in range(self.nroot):
            off = self.root_off + slot * DIR_ENTRY_SIZE
            if self.raw[off] == END_OF_DIR:
                return off
        raise ImageError("%s: root directory is full (%d entries); delete a file first"
                         % (self.name, self.nroot))

    # -- file operations ---------------------------------------------------
    def read_file(self, dosname):
        ent = self.find(dosname)
        if ent is None:
            raise ImageError("%s: %s not found" % (self.name, dosname))
        _, _, name, _, cl, size = ent
        if size == 0:
            return name, b""
        chain = self.chain_of(cl)
        chunks = [bytes(self.raw[self.cluster_off(c):self.cluster_off(c) + self.cluster_bytes])
                  for c in chain]
        return name, b"".join(chunks)[:size]

    def delete_file(self, dosname):
        """Free a file's clusters and mark its slot deleted.  Returns bytes freed."""
        ent = self.find(dosname)
        if ent is None:
            return None
        _, off, name, _, cl, size = ent
        if size:
            for c in self.chain_of(cl):
                self.write_fat(c, 0)
        self.raw[off] = FREE
        return size

    def put_file(self, dosname, data, mtime_now=False, replace=True):
        """Create or replace a file.  Returns (name, clusters used, action)."""
        base, ext = split_83(dosname)
        full = join_83(base, ext)
        data = bytes(data)
        need = max(1, (len(data) + self.cluster_bytes - 1) // self.cluster_bytes)

        existing = self.find(full)
        slot_off = None
        action = "added"
        freed = 0
        if existing is not None:
            if not replace:
                raise ImageError("%s already exists on %s (use --force to replace)"
                                 % (full, self.name))
            _, slot_off, _, _, cl, size = existing
            if size:
                for c in self.chain_of(cl):
                    self.write_fat(c, 0)
                    freed += 1
            action = "replaced"

        free = self.free_clusters()
        if need > len(free):
            raise ImageError(
                "not enough space for %s: need %d cluster(s) (%d bytes), only %d free "
                "(%d bytes). Drop files with --drop or start from a bigger DOS image."
                % (full, need, need * self.cluster_bytes, len(free),
                   len(free) * self.cluster_bytes))

        chain = free[:need]
        for i, cl in enumerate(chain):
            self.write_fat(cl, chain[i + 1] if i + 1 < len(chain) else 0xFFF)
            off = self.cluster_off(cl)
            piece = data[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
            self.raw[off:off + self.cluster_bytes] = piece.ljust(self.cluster_bytes, b"\x00")

        if slot_off is None:
            slot_off = self._slot_for_new_entry()

        t, d = dos_datetime(fixed=not mtime_now)
        entry = bytearray(DIR_ENTRY_SIZE)
        entry[0:8] = base
        entry[8:11] = ext
        entry[11] = ATTR_ARCHIVE
        struct.pack_into("<H", entry, 22, t)
        struct.pack_into("<H", entry, 24, d)
        struct.pack_into("<H", entry, 26, chain[0])
        struct.pack_into("<I", entry, 28, len(data))
        self.raw[slot_off:slot_off + DIR_ENTRY_SIZE] = entry
        return full, need, action, freed

    # -- reporting ---------------------------------------------------------
    def free_bytes(self):
        return len(self.free_clusters()) * self.cluster_bytes

    def used_bytes(self):
        return sum(ent[5] for ent in self.files())

    def geometry_label(self):
        geo = SUPPORTED_GEOMETRY.get(len(self.raw))
        return geo[3] if geo else "UNSUPPORTED size"

    def save(self, path):
        with open(path, "wb") as fh:
            fh.write(bytes(self.raw))


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------
def sha256_of(data):
    return hashlib.sha256(data).hexdigest()


def load_image(path):
    if not os.path.isfile(path):
        raise ImageError("%s: no such file" % path)
    im = Fat12Image(path)
    if im.size_mismatch:
        print("warning: %s holds %d bytes but its BPB describes %d bytes (%d sectors x %d)"
              % (path, im.size_mismatch[1], im.size_mismatch[0],
                 im.total_sectors, im.bps), file=sys.stderr)
    return im


def cmd_info(args):
    im = load_image(args.image)
    geo = SUPPORTED_GEOMETRY.get(len(im.raw))
    print("file          : %s" % args.image)
    print("size          : %d bytes (%.3f KiB)" % (len(im.raw), len(im.raw) / 1024.0))
    print("OEM           : %r" % im.oem)
    print("jump          : %s" % im.jump.hex())
    print("bytes/sector  : %d" % im.bps)
    print("sectors/clust : %d  (%d bytes per cluster)" % (im.spc, im.cluster_bytes))
    print("reserved      : %d" % im.reserved)
    print("#FATs         : %d" % im.nfats)
    print("root entries  : %d" % im.nroot)
    print("total sectors : %d" % im.total_sectors)
    print("media byte    : 0x%02X" % im.media)
    print("sectors/FAT   : %d" % im.spf)
    print("sectors/track : %d" % im.spt)
    print("heads         : %d" % im.heads)
    print("data clusters : %d" % im.data_clusters)
    print("free          : %d clusters = %d bytes" % (len(im.free_clusters()), im.free_bytes()))
    print("free dir slots: %d" % im.free_slots())
    files = im.files()
    print("files         : %d (%d bytes of data)" % (len(files), im.used_bytes()))
    owned, leaked, crossed, bad_eof = im.audit()
    print("clusters      : %d owned, %d leaked, %d cross-linked"
          % (len(owned), len(leaked), len(crossed)))
    if leaked:
        print("                %d cluster(s) are marked in use but no file owns them"
              % len(leaked))
        print("                (%d bytes lost); the allocator will not touch them."
              % (len(leaked) * im.cluster_bytes))
    if crossed:
        print("                WARNING: cross-linked clusters - the directory is damaged")
    if bad_eof:
        print("                WARNING: %d file(s) whose cluster chain has no end marker"
              % bad_eof)
    if geo:
        print("emulator geom : %d heads / %d sectors/track / %d cylinders  [%s: supported]"
              % (geo[0], geo[1], geo[2], geo[3]))
    else:
        print("emulator geom : NOT SUPPORTED by CardPuter86.")
        print("                Supported sizes: %s"
              % ", ".join("%s=%d" % (v[3], k)
                          for k, v in sorted(SUPPORTED_GEOMETRY.items())))
        print("                The emulator would treat this as a hard disk and DOS")
        print("                could not boot from it.")
    differing = im.fat_copies_differ()
    if differing is not None:
        print("FAT copies    : MISMATCH starting at cluster %d" % differing)
    elif im.nfats > 1:
        print("FAT copies    : identical")
    return 0


def cmd_list(args):
    im = load_image(args.image)
    files = im.files()
    if not files:
        print("(no files)")
        return 0
    print("%-12s %10s  %6s  %s" % ("NAME", "BYTES", "1st CL", "CONTIGUOUS"))
    total = 0
    for slot, off, name, attr, cl, size in files:
        total += size
        if size:
            chain = im.chain_of(cl)
            contiguous = all(chain[i] + 1 == chain[i + 1] for i in range(len(chain) - 1))
            flag = "yes" if contiguous else "no (%d clusters)" % len(chain)
        else:
            flag = "empty"
        print("%-12s %10d  %6d  %s" % (name, size, cl, flag))
    print("-" * 48)
    print("%d file(s), %d bytes; %d bytes free"
          % (len(files), total, im.free_bytes()))
    return 0


def cmd_add(args):
    im = load_image(args.image)
    with open(args.local, "rb") as fh:
        data = fh.read()
    full, clusters, action, freed = im.put_file(args.dosname, data,
                                                mtime_now=args.mtime_now,
                                                replace=args.force)
    name, back = im.read_file(full)
    if back != data:
        raise ImageError("read-back mismatch for %s - image NOT saved" % full)
    im.save(args.image)
    print("%s %s: %d bytes, %d cluster(s)%s"
          % (action, full, len(data), clusters,
             ", freed %d old cluster(s)" % freed if freed else ""))
    print("verified: %s reads back byte-identical (sha256 %s)"
          % (full, sha256_of(data)[:16]))
    return 0


def cmd_del(args):
    im = load_image(args.image)
    removed = 0
    for name in args.dosname:
        freed = im.delete_file(name)
        if freed is None:
            print("not found: %s" % name, file=sys.stderr)
            continue
        print("deleted %s (%d bytes freed)" % (name.upper(), freed))
        removed += 1
    if not removed:
        raise ImageError("nothing was deleted; image NOT saved")
    im.save(args.image)
    print("saved %s (%d bytes free)" % (args.image, im.free_bytes()))
    return 0


def cmd_get(args):
    im = load_image(args.image)
    name, data = im.read_file(args.dosname)
    with open(args.outfile, "wb") as fh:
        fh.write(data)
    print("%s -> %s (%d bytes, sha256 %s)"
          % (name, args.outfile, len(data), sha256_of(data)[:16]))
    return 0


def cmd_verify(args):
    im = load_image(args.image)
    with open(args.local, "rb") as fh:
        want = fh.read()
    name, got = im.read_file(args.dosname)
    ok = got == want
    print("%s: image=%d bytes, local=%d bytes" % (name, len(got), len(want)))
    print("image sha256 %s" % sha256_of(got))
    print("local sha256 %s" % sha256_of(want))
    print("BYTE-IDENTICAL:", ok)
    if not ok:
        n = min(len(got), len(want))
        for i in range(n):
            if got[i] != want[i]:
                print("first difference at offset %d: image=0x%02X local=0x%02X"
                      % (i, got[i], want[i]))
                break
        else:
            print("first difference at offset %d: one side just ends there" % n)
        return 1
    return 0


DEFAULT_AUTOEXEC = [
    "@echo off",
    "prompt $P$G",
]


def collect_apps(app_dir, explicit):
    """Return [(dosname, localpath)] from a directory plus explicit pairs."""
    out = []
    seen = set()
    if app_dir:
        if not os.path.isdir(app_dir):
            raise ImageError("--apps %s: not a directory" % app_dir)
        for entry in sorted(os.listdir(app_dir)):
            if entry.startswith("."):
                continue
            full = os.path.join(app_dir, entry)
            if not os.path.isfile(full):
                continue
            if os.path.splitext(entry)[1].upper() in (".CPP", ".C", ".H", ".MD", ".TXT",
                                                      ".OBJ", ".PY", ".BAT", ".ASM"):
                # Sources and notes are shipped for reference, not for the disk.
                continue
            split_83(entry)  # fail early on a bad name, before any write
            out.append((entry.upper(), full))
            seen.add(entry.upper())
    for spec in explicit or []:
        if "=" not in spec:
            raise ImageError("--add wants DOSNAME=LOCALFILE, got %r" % spec)
        dosname, local = spec.split("=", 1)
        split_83(dosname)
        if not os.path.isfile(local):
            raise ImageError("--add %s: %s is not a file" % (dosname, local))
        if dosname.upper() in seen:
            out = [item for item in out if item[0] != dosname.upper()]
        out.append((dosname.upper(), local))
        seen.add(dosname.upper())
    return out


def cmd_build(args):
    im = load_image(args.base)
    print("== base ==")
    print("  %s: %d bytes, %s, %d bytes free"
          % (args.base, len(im.raw), im.geometry_label(), im.free_bytes()))
    geo = SUPPORTED_GEOMETRY.get(len(im.raw))
    if geo is None:
        raise ImageError(
            "%s is %d bytes, which CardPuter86 cannot boot: the emulator only knows "
            "%s. Re-create the image at a supported size."
            % (args.base, len(im.raw),
               ", ".join("%s(%d)" % (v[3], k) for k, v in sorted(SUPPORTED_GEOMETRY.items()))))
    if im.oem.upper().startswith(("EXFAT", "NTFS")):
        raise ImageError("%s: OEM %r looks like exFAT/NTFS, not FAT12"
                         % (args.base, im.oem))

    # ---- plan deletions ---------------------------------------------------
    drops = []
    if args.strip_installer:
        drops.extend(INSTALLER_PAYLOAD)
    for item in args.drop or []:
        drops.extend(part for part in item.replace(";", ",").split(",") if part.strip())
    doomed = []
    for name in drops:
        name = name.strip().upper()
        ent = im.find(name) if name else None
        if ent is not None:
            doomed.append((ent[2], ent[5]))

    # ---- plan additions ---------------------------------------------------
    # --no-apps suppresses the apps directory only; explicit --add always wins.
    if args.no_apps:
        apps = collect_apps(None, args.add)
    else:
        apps = collect_apps(args.apps, args.add)
    if args.autobat_file:
        with open(args.autobat_file, "r", encoding="ascii", newline="") as fh:
            autobat = fh.read()
        autobat_replace = True
    elif args.no_autobat:
        autobat, autobat_replace = None, False
    else:
        lines = list(DEFAULT_AUTOEXEC)
        if args.path is not None and args.path != "":
            lines.append("path %s" % args.path)
        if args.extra_bat:
            lines.extend(args.extra_bat)
        autobat = "\r\n".join(lines) + "\r\n"
        autobat_replace = True

    # ---- space accounting, before writing anything ------------------------
    # Space is won and lost in whole clusters, so the projection must count
    # clusters - using raw file sizes makes the "free after build" number a lie.
    def clusters_for(size_bytes):
        return max(1, (size_bytes + im.cluster_bytes - 1) // im.cluster_bytes)

    freed_clusters = 0
    for name, size in doomed:
        ent = im.find(name)
        if ent is not None and ent[5]:
            freed_clusters += len(im.chain_of(ent[4])) if ent[4] >= 2 else clusters_for(ent[5])
    reclaimed_clusters = 0
    doomed_names = set(d[0] for d in doomed)
    needed_clusters = 0
    for name, local in apps:
        needed_clusters += clusters_for(os.path.getsize(local))
        ent = im.find(name)
        if ent is not None and name not in doomed_names and ent[5]:
            reclaimed_clusters += len(im.chain_of(ent[4])) if ent[4] >= 2 else 0
    if autobat_replace:
        ent = im.find("AUTOEXEC.BAT")
        if ent is not None and ent[5]:
            reclaimed_clusters += len(im.chain_of(ent[4])) if ent[4] >= 2 else 0
    autobat_clusters = clusters_for(len(autobat.encode("ascii"))) if autobat else 0
    projected_clusters = (len(im.free_clusters()) + freed_clusters +
                          reclaimed_clusters - needed_clusters - autobat_clusters)
    projected = projected_clusters * im.cluster_bytes
    print("== plan ==")
    for name, size in doomed:
        print("  drop   %-12s %8d bytes" % (name, size))
    for name, local in apps:
        print("  put    %-12s %8d bytes  <- %s"
              % (name, os.path.getsize(local), os.path.basename(local)))
    if autobat_replace:
        print("  write  %-12s %8d bytes  (prompt/path)" % ("AUTOEXEC.BAT", len(autobat)))
    print("  free after build: %d bytes (%d clusters)" % (projected, projected_clusters))
    if projected_clusters < 0:
        raise ImageError(
            "build would not fit: short by %d cluster(s) (%d bytes). Add more --drop "
            "names or start from a larger DOS image."
            % (-projected_clusters, -projected))
    if args.min_free and projected < args.min_free:
        raise ImageError("free space after build would be %d bytes, below --min-free %d"
                         % (projected, args.min_free))

    # ---- apply ------------------------------------------------------------
    for name, _ in doomed:
        im.delete_file(name)
    for name, local in apps:
        with open(local, "rb") as fh:
            data = fh.read()
        im.put_file(name, data, mtime_now=args.mtime_now, replace=True)
    if autobat is not None:
        im.put_file("AUTOEXEC.BAT", autobat.encode("ascii"), mtime_now=args.mtime_now)

    # ---- verify read-back of everything we touched ------------------------
    print("== verify ==")
    problems = []
    for name, local in apps:
        with open(local, "rb") as fh:
            want = fh.read()
        _, got = im.read_file(name)
        ok = got == want
        print("  %-12s %s  (%d bytes)" % (name, "OK" if ok else "MISMATCH", len(got)))
        if not ok:
            problems.append(name)
    if autobat is not None:
        _, got = im.read_file("AUTOEXEC.BAT")
        ok = got == autobat.encode("ascii")
        print("  %-12s %s  (%d bytes)" % ("AUTOEXEC.BAT", "OK" if ok else "MISMATCH",
                                          len(got)))
        if not ok:
            problems.append("AUTOEXEC.BAT")
    differing = im.fat_copies_differ()
    if differing is not None:
        problems.append("FAT copies differ at cluster %d" % differing)
        print("  FAT copies    MISMATCH at cluster %d" % differing)
    elif im.nfats > 1:
        print("  FAT copies    OK  (all %d in sync)" % im.nfats)
    if problems:
        raise ImageError("verification failed (%s) - image NOT saved" % ", ".join(problems))

    # ---- save -------------------------------------------------------------
    out = args.out
    if os.path.abspath(out) == os.path.abspath(args.base) and not args.in_place:
        raise ImageError("--out points at --base; pass --in-place if you really mean it")
    if os.path.exists(out) and os.path.abspath(out) != os.path.abspath(args.base) \
            and not args.force:
        raise ImageError("%s already exists (use --force to overwrite)" % out)
    im.save(out)
    print("== done ==")
    print("  wrote %s: %d bytes, %d files, %d bytes free"
          % (out, len(im.raw), len(im.files()), im.free_bytes()))
    print("  sha256 %s" % hashlib.sha256(bytes(im.raw)).hexdigest())
    print()
    print("  Next: copy it to data/cardputer86.img in the project and run")
    print("        pio run -t buildfs -e cardputer86-lean")
    print("        then flash fatfs.bin at 0x2A1000 (see docs/IMAGE.md).")
    return 0


# --------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="cp86img.py",
        description="Build a bootable CardPuter86 floppy image from your own DOS disk.",
        epilog="Supported image sizes: " +
               ", ".join("%s (%d bytes)" % (v[3], k)
                         for k, v in sorted(SUPPORTED_GEOMETRY.items())))
    ap.add_argument("--version", action="version", version="%(prog)s " + VERSION)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="show BPB geometry, free space and FAT health")
    p.add_argument("image")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("list", help="list the root directory")
    p.add_argument("image")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("build", help="produce a finished CardPuter86 system image")
    p.add_argument("--base", required=True, help="your own DOS boot image (never modified)")
    p.add_argument("--out", required=True, help="image to write")
    p.add_argument("--apps", default=None,
                   help="directory of guest programs to inject (default: ./apps)")
    p.add_argument("--no-apps", action="store_true", help="do not inject any programs")
    p.add_argument("--add", action="append", metavar="DOSNAME=FILE",
                   help="inject one extra file (repeatable)")
    p.add_argument("--drop", action="append", metavar="NAME[,NAME...]",
                   help="delete these files (repeatable)")
    p.add_argument("--strip-installer", action="store_true",
                   help="drop the DOS retail installer payload (%s)"
                        % ", ".join(INSTALLER_PAYLOAD))
    p.add_argument("--path", default="A:\\;C:\\;C:\\CPP86",
                   help="PATH for AUTOEXEC.BAT (default: A:\\;C:\\;C:\\CPP86; '' to omit)")
    p.add_argument("--extra-bat", action="append", metavar="LINE",
                   help="extra AUTOEXEC.BAT line (repeatable)")
    p.add_argument("--autobat-file", default=None,
                   help="use this file as AUTOEXEC.BAT instead of generating one")
    p.add_argument("--no-autobat", action="store_true",
                   help="leave AUTOEXEC.BAT completely alone")
    p.add_argument("--min-free", type=int, default=0,
                   help="fail unless at least this many bytes stay free")
    p.add_argument("--mtime-now", action="store_true",
                   help="stamp entries with the current time instead of a fixed date")
    p.add_argument("--force", action="store_true", help="overwrite --out if it exists")
    p.add_argument("--in-place", action="store_true", help="allow --out == --base")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("add", help="add or replace a single file")
    p.add_argument("image")
    p.add_argument("dosname")
    p.add_argument("local")
    p.add_argument("--force", action="store_true", help="replace an existing file")
    p.add_argument("--mtime-now", action="store_true")
    p.set_defaults(func=cmd_add)

    p = sub.add_parser("del", help="delete files")
    p.add_argument("image")
    p.add_argument("dosname", nargs="+")
    p.set_defaults(func=cmd_del)

    p = sub.add_parser("get", help="extract a file")
    p.add_argument("image")
    p.add_argument("dosname")
    p.add_argument("outfile")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("verify", help="compare a file inside the image with a local file")
    p.add_argument("image")
    p.add_argument("dosname")
    p.add_argument("local")
    p.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    # Default --apps to ./apps next to this script when the user did not say.
    if args.cmd == "build" and args.apps is None:
        here = os.path.dirname(os.path.abspath(__file__))
        candidate = os.path.join(here, "apps")
        args.apps = candidate if os.path.isdir(candidate) else None
        if args.apps is None:
            print("note: no apps/ directory next to cp86img.py, injecting nothing",
                  file=sys.stderr)
    try:
        return args.func(args)
    except ImageError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main())
