"""Minimal MS-CFB (Compound File Binary Format) writer.

Enough of the spec to produce files that Outlook / olefile / extract-msg accept.
Version 3: 512-byte sectors, 64-byte mini sectors, 4096-byte mini cutoff.
"""
import struct

FREESECT   = 0xFFFFFFFF
ENDOFCHAIN = 0xFFFFFFFE
FATSECT    = 0xFFFFFFFD
DIFSECT    = 0xFFFFFFFC
NOSTREAM   = 0xFFFFFFFF

SECTOR = 512
MINI_SECTOR = 64
MINI_CUTOFF = 4096


class Entry:
    """A directory entry: storage (dir) or stream (file)."""

    def __init__(self, name, is_stream, data=b"", clsid=b"\x00" * 16):
        self.name = name
        self.is_stream = is_stream
        self.data = data
        self.clsid = clsid
        self.children = []
        # filled in during layout
        self.id = None
        self.left = NOSTREAM
        self.right = NOSTREAM
        self.child = NOSTREAM
        self.start = ENDOFCHAIN
        self.size = 0

    def add(self, entry):
        self.children.append(entry)
        return entry


def _name_key(name):
    """MS-CFB sibling ordering: by UTF-16 length first, then uppercase."""
    return (len(name), name.upper())


def _build_tree(entries):
    """Balanced BST from sorted siblings. Returns root id, all colored black."""
    if not entries:
        return NOSTREAM
    entries = sorted(entries, key=lambda e: _name_key(e.name))

    def rec(lo, hi):
        if lo > hi:
            return NOSTREAM
        mid = (lo + hi) // 2
        e = entries[mid]
        e.left = rec(lo, mid - 1)
        e.right = rec(mid + 1, hi)
        return e.id

    return rec(0, len(entries) - 1)


def _chunk(data, size):
    return [data[i:i + size].ljust(size, b"\x00")
            for i in range(0, len(data), size)]


def write(root, path):
    """Serialize a root Entry (a storage) and its tree to `path`."""
    # --- assign ids depth-first so parents precede children ---
    flat = []

    def walk(e):
        e.id = len(flat)
        flat.append(e)
        for c in e.children:
            walk(c)

    walk(root)

    # --- split streams into mini vs regular ---
    mini_streams = []
    big_streams = []
    for e in flat:
        if not e.is_stream:
            continue
        e.size = len(e.data)
        if e.size == 0:
            continue
        (mini_streams if e.size < MINI_CUTOFF else big_streams).append(e)

    # --- build the mini stream and mini FAT ---
    ministream = bytearray()
    minifat = []
    for e in mini_streams:
        sectors = _chunk(e.data, MINI_SECTOR)
        e.start = len(minifat)
        for i in range(len(sectors)):
            minifat.append(len(minifat) + 1)
        minifat[-1] = ENDOFCHAIN
        for s in sectors:
            ministream += s

    minifat_bytes = struct.pack("<%dI" % len(minifat), *minifat) if minifat else b""
    minifat_sectors = _chunk(minifat_bytes, SECTOR) if minifat_bytes else []
    # pad the tail of the minifat sector with FREESECT, not zeros
    if minifat_sectors:
        tail = len(minifat_bytes) % SECTOR
        if tail:
            last = bytearray(minifat_sectors[-1])
            for off in range(tail, SECTOR, 4):
                last[off:off + 4] = struct.pack("<I", FREESECT)
            minifat_sectors[-1] = bytes(last)

    # --- directory entries ---
    root.child = _build_tree(root.children)
    for e in flat:
        if not e.is_stream:
            e.child = _build_tree(e.children)

    ministream_sectors = _chunk(bytes(ministream), SECTOR)

    # --- iteratively size the FAT (its own sectors count toward the total) ---
    n_dir_entries = len(flat)
    dir_sectors_count = (n_dir_entries + 3) // 4  # 4 entries of 128B per sector

    payload = len(ministream_sectors) + len(minifat_sectors) + dir_sectors_count
    for e in big_streams:
        payload += (len(e.data) + SECTOR - 1) // SECTOR

    n_fat = 1
    n_difat = 0
    while True:
        total = payload + n_fat + n_difat
        need_fat = max(1, (total + 127) // 128)
        need_difat = 0
        if need_fat > 109:
            # each DIFAT sector holds 127 FAT pointers + a next-pointer
            need_difat = ((need_fat - 109) + 126) // 127
        if need_fat == n_fat and need_difat == n_difat:
            break
        n_fat, n_difat = need_fat, need_difat

    # --- allocate sector numbers in layout order ---
    next_sec = 0

    def alloc(n):
        nonlocal next_sec
        first = next_sec
        next_sec += n
        return first

    fat = {}

    def chain(first, count):
        for i in range(count):
            fat[first + i] = first + i + 1
        fat[first + count - 1] = ENDOFCHAIN

    ministream_start = ENDOFCHAIN
    if ministream_sectors:
        ministream_start = alloc(len(ministream_sectors))
        chain(ministream_start, len(ministream_sectors))

    for e in big_streams:
        n = (len(e.data) + SECTOR - 1) // SECTOR
        e.start = alloc(n)
        chain(e.start, n)

    minifat_start = ENDOFCHAIN
    if minifat_sectors:
        minifat_start = alloc(len(minifat_sectors))
        chain(minifat_start, len(minifat_sectors))

    dir_start = alloc(dir_sectors_count)
    chain(dir_start, dir_sectors_count)

    fat_start = alloc(n_fat)
    for i in range(n_fat):
        fat[fat_start + i] = FATSECT

    difat_start = ENDOFCHAIN
    if n_difat:
        difat_start = alloc(n_difat)
        for i in range(n_difat):
            fat[difat_start + i] = DIFSECT

    total_sectors = next_sec

    # --- root entry points at the mini stream ---
    root.start = ministream_start
    root.size = len(ministream)

    # --- serialize directory ---
    dir_bytes = bytearray()
    for e in flat:
        nm = e.name.encode("utf-16-le") + b"\x00\x00"
        if len(nm) > 64:
            raise ValueError("directory name too long: %r" % e.name)
        entry = bytearray(128)
        entry[0:len(nm)] = nm
        struct.pack_into("<H", entry, 64, len(nm))
        if e is root:
            obj_type = 5           # root storage
        elif e.is_stream:
            obj_type = 2           # stream
        else:
            obj_type = 1           # storage
        entry[66] = obj_type
        entry[67] = 1              # black
        struct.pack_into("<I", entry, 68, e.left)
        struct.pack_into("<I", entry, 72, e.right)
        struct.pack_into("<I", entry, 76, e.child)
        entry[80:96] = e.clsid
        struct.pack_into("<I", entry, 116, e.start)
        struct.pack_into("<Q", entry, 120, e.size)
        dir_bytes += entry
    dir_sectors = _chunk(bytes(dir_bytes), SECTOR)
    # unused slots in the last directory sector must be marked empty
    if n_dir_entries % 4:
        last = bytearray(dir_sectors[-1])
        for slot in range(n_dir_entries % 4, 4):
            off = slot * 128
            struct.pack_into("<I", last, off + 68, NOSTREAM)
            struct.pack_into("<I", last, off + 72, NOSTREAM)
            struct.pack_into("<I", last, off + 76, NOSTREAM)
        dir_sectors[-1] = bytes(last)

    # --- serialize the FAT ---
    fat_entries = [fat.get(i, FREESECT) for i in range(total_sectors)]
    fat_entries += [FREESECT] * (n_fat * 128 - len(fat_entries))
    fat_bytes = struct.pack("<%dI" % len(fat_entries), *fat_entries)
    fat_sectors = _chunk(fat_bytes, SECTOR)

    # --- DIFAT ---
    fat_locs = [fat_start + i for i in range(n_fat)]
    header_difat = fat_locs[:109] + [FREESECT] * (109 - len(fat_locs[:109]))
    difat_sectors = []
    if n_difat:
        rest = fat_locs[109:]
        for i in range(n_difat):
            block = rest[i * 127:(i + 1) * 127]
            block += [FREESECT] * (127 - len(block))
            nxt = difat_start + i + 1 if i + 1 < n_difat else ENDOFCHAIN
            difat_sectors.append(struct.pack("<128I", *(block + [nxt])))

    # --- header ---
    hdr = bytearray(512)
    hdr[0:8] = b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"
    struct.pack_into("<H", hdr, 24, 0x003E)   # minor version
    struct.pack_into("<H", hdr, 26, 0x0003)   # major version
    struct.pack_into("<H", hdr, 28, 0xFFFE)   # little endian
    struct.pack_into("<H", hdr, 30, 9)        # sector shift -> 512
    struct.pack_into("<H", hdr, 32, 6)        # mini sector shift -> 64
    struct.pack_into("<I", hdr, 40, 0)        # dir sector count (v3: 0)
    struct.pack_into("<I", hdr, 44, n_fat)
    struct.pack_into("<I", hdr, 48, dir_start)
    struct.pack_into("<I", hdr, 56, MINI_CUTOFF)
    struct.pack_into("<I", hdr, 60, minifat_start)
    struct.pack_into("<I", hdr, 64, len(minifat_sectors))
    struct.pack_into("<I", hdr, 68, difat_start)
    struct.pack_into("<I", hdr, 72, n_difat)
    for i, v in enumerate(header_difat):
        struct.pack_into("<I", hdr, 76 + i * 4, v)

    # --- assemble in allocation order ---
    out = bytearray(hdr)
    blank = b"\x00" * SECTOR
    sectors = [blank] * total_sectors

    def place(first, chunks):
        for i, c in enumerate(chunks):
            sectors[first + i] = c

    if ministream_sectors:
        place(ministream_start, ministream_sectors)
    for e in big_streams:
        place(e.start, _chunk(e.data, SECTOR))
    if minifat_sectors:
        place(minifat_start, minifat_sectors)
    place(dir_start, dir_sectors)
    place(fat_start, fat_sectors)
    if difat_sectors:
        place(difat_start, difat_sectors)

    for s in sectors:
        out += s

    with open(path, "wb") as f:
        f.write(out)
    return len(out)
