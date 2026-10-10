"""Copy saves from a PCSX2 / PS2 memory card image (.ps2) into this runtime's
memory card (saves/card0.ps2mc).

    python tools/import_pcsx2_save.py Mcd001.ps2 saves/card0.ps2mc
    python tools/import_pcsx2_save.py Mcd001.ps2 saves/card0.ps2mc --list

By default every save folder for SLUS-20851 is copied.  A folder the runtime
card already has is left alone unless --replace is given.  The PCSX2 card is
only read; the runtime card is backed up before it is rewritten.  Only the
standard library is used.
"""
import argparse
import math
import os
import shutil
import struct
import sys
import time

PS2_MAGIC = b"Sony PS2 Memory Card Format "
RT_MAGIC = b"AC5MC01\0"
RT_ENTRIES = 256
RT_BYTES = 8 * 1024 * 1024
RT_CLUSTERS = 8192


# ---- PS2 memory card image (read only) ----------------------------------

class Ps2Card:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.raw = f.read()
        sb = self.raw[:512]
        if not sb.startswith(PS2_MAGIC):
            raise SystemExit("%s is not a formatted PS2 memory card image" % path)
        (self.page_len, self.ppc, _, _, _, self.alloc_off, _,
         self.root) = struct.unpack_from("<HHHHIIII", sb, 40)
        self.ifc = struct.unpack_from("<32I", sb, 80)
        pages = 16384 if len(self.raw) >= 16384 * self.page_len else len(self.raw) // self.page_len
        self.stride = len(self.raw) // pages        # 528 with ECC, 512 without
        self.csize = self.page_len * self.ppc

    def cluster(self, n):
        return b"".join(self.raw[p * self.stride:p * self.stride + self.page_len]
                        for p in range(n * self.ppc, (n + 1) * self.ppc))

    def fat(self, rel):
        per = self.csize // 4
        ind = rel // per
        fat_cluster = struct.unpack_from("<I", self.cluster(self.ifc[ind // per]), (ind % per) * 4)[0]
        return struct.unpack_from("<I", self.cluster(fat_cluster), (rel % per) * 4)[0]

    def read(self, rel, size):
        out, seen = [], set()
        while len(out) * self.csize < size:
            if rel in seen:
                raise SystemExit("the PS2 card's FAT loops at cluster %d" % rel)
            seen.add(rel)
            out.append(self.cluster(rel + self.alloc_off))
            e = self.fat(rel)
            if (e & 0x7FFFFFFF) == 0x7FFFFFFF:
                break
            if not e & 0x80000000:
                raise SystemExit("the PS2 card's FAT runs into a free cluster at %d" % rel)
            rel = e & 0x7FFFFFFF
        data = b"".join(out)
        if len(data) < size:
            raise SystemExit("a file on the PS2 card is shorter than its directory entry says")
        return data[:size]

    @staticmethod
    def entry(b):
        mode, _, length = struct.unpack_from("<HHI", b, 0)
        return dict(mode=mode, length=length, created=b[8:16],
                    cluster=struct.unpack_from("<I", b, 16)[0], modified=b[24:32],
                    name=b[64:96].split(b"\0", 1)[0].decode("ascii", "replace"))

    def listdir(self, d):
        data = self.read(d["cluster"], d["length"] * 512)
        items = (self.entry(data[i * 512:(i + 1) * 512]) for i in range(d["length"]))
        return [e for e in items if e["mode"] & 0x8000 and e["name"] not in (".", "..")]

    def root_dir(self):
        return self.entry(self.read(self.root, 512))


# ---- this runtime's card (runtime/src/ps2_memcard.c) ----------------------

def stamp_now():
    t = time.localtime()
    return bytes([0, t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon]) + struct.pack("<H", t.tm_year)


def info_block(created, modified, size, mode):
    info = bytearray(64)
    info[0:8] = created
    info[8:16] = modified
    struct.pack_into("<IH", info, 16, size, mode)
    return info


def load_runtime(path):
    if not os.path.exists(path):
        return 1, [("/", info_block(stamp_now(), stamp_now(), 0, 0x8027), b"")]
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] != RT_MAGIC:
        raise SystemExit("%s is not one of this runtime's memory cards" % path)
    count, formatted = struct.unpack_from("<II", raw, 8)
    pos, entries = 16, []
    for _ in range(count):
        name = raw[pos:pos + 256].split(b"\0", 1)[0].decode("ascii")
        info = bytearray(raw[pos + 256:pos + 320])
        size = struct.unpack_from("<I", info, 16)[0]
        pos += 320
        entries.append((name, info, raw[pos:pos + size]))
        pos += size
    if pos != len(raw):
        raise SystemExit("%s has data after its last entry; refusing to touch it" % path)
    return formatted, entries


def check_runtime(formatted, entries):
    """The checks ps2_memcard.c's load_card makes before it accepts a card."""
    names = [e[0] for e in entries]
    if len(entries) > RT_ENTRIES:
        raise SystemExit("more than %d entries; the runtime refuses that card" % RT_ENTRIES)
    if len(set(names)) != len(names) or any(not n.startswith("/") or len(n) >= 256 for n in names):
        raise SystemExit("duplicate or invalid path on the card")
    if sum(len(d) for _, _, d in entries) > RT_BYTES:
        raise SystemExit("more than 8 MB of data; the runtime refuses that card")
    if formatted and "/" not in names:
        raise SystemExit("the card has no root directory")
    if 16 + sum(1 + math.ceil(len(d) / 1024) for _, _, d in entries) > RT_CLUSTERS:
        raise SystemExit("the card would be over its 8192-cluster capacity")


def write_runtime(path, formatted, entries):
    out = bytearray(RT_MAGIC + struct.pack("<II", len(entries), formatted))
    for name, info, data in entries:
        struct.pack_into("<I", info, 16, len(data))
        out += name.encode("ascii").ljust(256, b"\0") + info + data
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)


def game_running(card_path):
    """The runtime holds card*.ps2mc.lock open with no sharing while it runs."""
    lock = card_path + ".lock"
    if not os.path.exists(lock):
        return False
    try:
        with open(lock, "a"):
            return False
    except PermissionError:
        return True


def main():
    ap = argparse.ArgumentParser(prog="import_pcsx2_save", description=__doc__.split("\n\n")[0])
    ap.add_argument("ps2_card", help="PCSX2 memory card image, e.g. Mcd001.ps2")
    ap.add_argument("runtime_card", help="this runtime's card, e.g. saves/card0.ps2mc")
    ap.add_argument("--save", action="append", help="save folder to copy (default: all SLUS-20851 ones)")
    ap.add_argument("--replace", action="store_true", help="overwrite a folder the runtime card already has")
    ap.add_argument("--list", action="store_true", help="only list the PS2 card's save folders")
    a = ap.parse_args()

    ps2 = Ps2Card(a.ps2_card)
    folders = [d for d in ps2.listdir(ps2.root_dir()) if d["mode"] & 0x20]
    if a.list:
        for d in folders:
            print(d["name"])
        return
    wanted = [d for d in folders if (d["name"] in a.save if a.save else "20851" in d["name"])]
    if not wanted:
        raise SystemExit("no matching save folder on %s" % a.ps2_card)
    if game_running(a.runtime_card):
        raise SystemExit("the game is running and holds %s; quit it first" % a.runtime_card)

    formatted, entries = load_runtime(a.runtime_card)
    for d in wanted:
        top = "/" + d["name"]
        if any(n == top or n.startswith(top + "/") for n, _, _ in entries):
            if not a.replace:
                raise SystemExit("%s is already on the runtime card; use --replace to overwrite it" % top)
            entries = [e for e in entries if e[0] != top and not e[0].startswith(top + "/")]
        entries.append((top, info_block(d["created"], d["modified"], 0, d["mode"]), b""))
        for f in ps2.listdir(d):
            if f["mode"] & 0x20:
                print("skipping subfolder %s/%s" % (top, f["name"]))
                continue
            data = ps2.read(f["cluster"], f["length"])
            entries.append((top + "/" + f["name"], info_block(f["created"], f["modified"], 0, f["mode"]), data))
            print("  %s/%s  %d bytes" % (top, f["name"], len(data)))
    check_runtime(formatted or 1, entries)

    if os.path.exists(a.runtime_card):
        backup = a.runtime_card + time.strftime(".bak-%Y%m%d-%H%M%S")
        shutil.copy2(a.runtime_card, backup)
        print("backed up the old card to %s" % backup)
    else:
        os.makedirs(os.path.dirname(os.path.abspath(a.runtime_card)), exist_ok=True)
    write_runtime(a.runtime_card, formatted or 1, entries)
    check_runtime(*load_runtime(a.runtime_card))
    print("wrote %s: %d entries" % (a.runtime_card, len(entries)))


if __name__ == "__main__":
    sys.exit(main())
