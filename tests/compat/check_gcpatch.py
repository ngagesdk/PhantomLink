#!/usr/bin/env python3
"""Tests for dll/variants/gcpatch.py (DLL variant patches in PhantomLink.sis).

Synthetic images: round trip for same size, growing and shrinking targets,
identical images and changes at the very start and end; damaged patches and
a wrong base must be rejected.  With --built DIR (containing vN/gamecomms.dll)
the real variants are checked against the base v6 as the SIS build does.
"""
import argparse
import os
import random
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.append(os.path.join(ROOT, 'dll', 'variants'))
import gcpatch  # noqa: E402

SHIPPED = [1, 2, 3, 5, 6, 7, 8, 9, 10, 11]  # app/CMakeLists.txt PL_ALL_VARIANTS
BASE = 6                                    # app/CMakeLists.txt PL_BASE_VARIANT


def expect_reject(base, patch, what):
    try:
        gcpatch.apply_patch(base, patch)
    except ValueError:
        return
    raise AssertionError('accepted: ' + what)


def synthetic():
    rnd = random.Random(1234)
    base = bytes(rnd.randrange(256) for _ in range(20000))
    cases = {'identical': base}
    t = bytearray(base)
    t[0] ^= 1
    t[-1] ^= 1
    cases['ends'] = bytes(t)
    t = bytearray(base)
    for off in (24, 5000, 5004, 9000):
        t[off:off + 40] = bytes(rnd.randrange(256) for _ in range(40))
    cases['same size'] = bytes(t)
    cases['grow'] = bytes(t) + b'\x01\x02\x03' + bytes(100)
    cases['shrink'] = bytes(t[:19000])
    for name, target in cases.items():
        patch = gcpatch.make_patch(BASE, base, target)
        assert gcpatch.apply_patch(base, patch) == target, name
        hdr = gcpatch.HEADER.size
        expect_reject(base[:-1], patch, name + ': shorter base')
        expect_reject(b'\x00' + base[1:] if base[0] else b'\x01' + base[1:], patch, name + ': other base')
        expect_reject(base, patch[:-1] if len(patch) > hdr else patch[:hdr - 1], name + ': truncated')
        expect_reject(base, b'XXXX' + patch[4:], name + ': magic')
        bad = bytearray(patch)
        bad[24:28] = struct.pack('<I', 0x7fffffff)
        expect_reject(base, bytes(bad), name + ': run count')
        if len(patch) > hdr:
            bad = bytearray(patch)
            bad[hdr:hdr + 4] = struct.pack('<I', len(target))   # run past the end
            expect_reject(base, bytes(bad), name + ': run offset')
            bad = bytearray(patch)
            bad[-1] ^= 0xff                                       # last data byte
            expect_reject(base, bytes(bad), name + ': run data')
    assert len(gcpatch.make_patch(BASE, base, base)) == gcpatch.HEADER.size
    print('synthetic: ok')


def built(dirname):
    gcpatch.cmd_check(dirname, BASE, SHIPPED)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--built', help='directory with vN/gamecomms.dll')
    args = ap.parse_args()
    synthetic()
    if args.built:
        built(args.built)
    return 0


if __name__ == '__main__':
    sys.exit(main())
