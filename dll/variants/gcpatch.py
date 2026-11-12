#!/usr/bin/env python3
"""Patch files for the GAMECOMMS.DLL variants in PhantomLink.sis.

All variants are built from the same code and differ only in their export
tables (and the header fields and relocations that go with them).  So the
SIS ships one full DLL, the base variant, plus a small patch per other
variant.  The application (app/src/GameInstaller.cpp) rebuilds the exact
variant on the phone when the user installs it for a game.

Patch format (.pat), all integers little-endian u32:

    0   magic "GCP1"
    4   base variant number
    8   base size
    12  base FNV-1a-32 hash
    16  target size
    20  target FNV-1a-32 hash
    24  run count
    28  runs: offset, length, <length> bytes

The target starts as the base, cut or zero-padded to the target size; each
run then overwrites <length> bytes at <offset>.

Usage:
    gcpatch.py make  BUILT_DIR OUT_DIR BASE V [V ...]
        BUILT_DIR/vN/gamecomms.dll -> OUT_DIR/gcvBASE.dll + OUT_DIR/gcvN.pat
    gcpatch.py apply BASE_DLL PATCH OUT_DLL
    gcpatch.py check BUILT_DIR BASE V [V ...]
        make the patches in memory, apply them and compare with the DLLs
"""
import os
import struct
import sys

MAGIC = b'GCP1'
HEADER = struct.Struct('<4s6I')
RUN = struct.Struct('<2I')
MAX_SIZE = 512 * 1024  # same limit as GameInstaller.cpp


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def diff_runs(base, target):
    """Byte ranges where target differs from base (base zero-padded).
    Runs closer together than one run header are merged."""
    padded = base[:len(target)].ljust(len(target), b'\0')
    runs = []
    i = 0
    n = len(target)
    while i < n:
        if padded[i] == target[i]:
            i += 1
            continue
        start = last = i
        while i < n and i - last <= RUN.size:
            if padded[i] != target[i]:
                last = i
            i += 1
        runs.append((start, last + 1))
    return runs


def make_patch(base_variant, base, target):
    runs = diff_runs(base, target)
    out = [HEADER.pack(MAGIC, base_variant, len(base), fnv1a(base),
                       len(target), fnv1a(target), len(runs))]
    for start, end in runs:
        out.append(RUN.pack(start, end - start))
        out.append(target[start:end])
    return b''.join(out)


def apply_patch(base, patch):
    if len(patch) < HEADER.size:
        raise ValueError('patch too short')
    magic, _bv, bsize, bhash, tsize, thash, nruns = HEADER.unpack_from(patch)
    if magic != MAGIC:
        raise ValueError('bad magic')
    if len(base) != bsize or fnv1a(base) != bhash:
        raise ValueError('base does not match the patch')
    if tsize > MAX_SIZE:
        raise ValueError('target too big')
    out = bytearray(base[:tsize].ljust(tsize, b'\0'))
    pos = HEADER.size
    for _ in range(nruns):
        if pos + RUN.size > len(patch):
            raise ValueError('truncated run header')
        off, n = RUN.unpack_from(patch, pos)
        pos += RUN.size
        if n > len(patch) - pos or off > tsize or n > tsize - off:
            raise ValueError('run out of range')
        out[off:off + n] = patch[pos:pos + n]
        pos += n
    if pos != len(patch) or fnv1a(out) != thash:
        raise ValueError('result does not match')
    return bytes(out)


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def write_if_changed(path, data):
    if os.path.exists(path) and read(path) == data:
        return
    with open(path, 'wb') as f:
        f.write(data)


def dll_path(built, v):
    return os.path.join(built, 'v%d' % v, 'gamecomms.dll')


def cmd_make(built, out_dir, base_v, variants):
    os.makedirs(out_dir, exist_ok=True)
    base = read(dll_path(built, base_v))
    write_if_changed(os.path.join(out_dir, 'gcv%d.dll' % base_v), base)
    total = len(base)
    for v in variants:
        if v == base_v:
            continue
        target = read(dll_path(built, v))
        patch = make_patch(base_v, base, target)
        if apply_patch(base, patch) != target:
            raise SystemExit('gcpatch: patch for v%d does not round-trip' % v)
        write_if_changed(os.path.join(out_dir, 'gcv%d.pat' % v), patch)
        total += len(patch)
    print('gcpatch: base v%d (%d bytes) + %d patches = %d bytes' %
          (base_v, len(base), len(variants) - (base_v in variants), total))


def cmd_check(built, base_v, variants):
    base = read(dll_path(built, base_v))
    for v in variants:
        if v == base_v:
            continue
        target = read(dll_path(built, v))
        patch = make_patch(base_v, base, target)
        assert apply_patch(base, patch) == target, 'v%d round-trip' % v
        # a damaged patch or a different base must be rejected
        for bad_base, bad_patch in ((base[:-1], patch),
                                    (base, patch[:-1]),
                                    (base, patch[:HEADER.size - 4] + b'\xff\xff\xff\x7f' + patch[HEADER.size:])):
            try:
                apply_patch(bad_base, bad_patch)
            except ValueError:
                pass
            else:
                raise AssertionError('v%d: damaged input accepted' % v)
        print('v%d: %d bytes patch, ok' % (v, len(patch)))
    print('all patches ok')


def main(argv):
    if len(argv) >= 5 and argv[1] == 'make':
        cmd_make(argv[2], argv[3], int(argv[4]), [int(v) for v in argv[5:]])
    elif len(argv) == 5 and argv[1] == 'apply':
        with open(argv[4], 'wb') as f:
            f.write(apply_patch(read(argv[2]), read(argv[3])))
    elif len(argv) >= 4 and argv[1] == 'check':
        cmd_check(argv[2], int(argv[3]), [int(v) for v in argv[4:]])
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
