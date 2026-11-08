#!/usr/bin/env python3
"""Classify how an N-Gage game uses GAMECOMMS.DLL.

usage: python tools/gc_usage.py <game.app | game folder> [...]

Prints the GameComms ordinals the game imports (with names from
dll/variants/variants.py), whether it carries its own Bluetooth stack,
the Bluetooth habits that matter for the replacement, and a verdict on
whether a GAMECOMMS.DLL replacement can carry its multiplayer at all.
See docs/COMPATIBILITY.md, "Why games fail".
"""
import os, re, struct, sys

def load_repo(repo):
    sys.path.insert(0, os.path.join(repo, 'tools', 're'))
    sys.path.insert(0, os.path.join(repo, 'dll', 'variants'))
    from e32 import E32
    import variants
    return E32, variants

GROUPS = {
    'session': {39, 40, 7, 8, 11, 32, 33, 14, 15, 36},   # 2.x core
    'helpers_disc': {28, 31, 57, 3, 24, 13, 35, 54, 62},   # discoverer
    'helpers_res': {25, 16, 50},                           # resolver
    'helpers_list': {26, 38, 21, 55, 23, 19, 10, 12, 4, 17, 18, 51},  # device list (inert)
}

def own_bt(e, data):
    dlls = {n.split('[')[0].split('.')[0].upper(): o for n, o in e.imports}
    hints = []
    for s in ('BTLinkManager', 'RFCOMM', 'L2CAP'):
        if s.encode('utf-16le') in data or s.encode() in data:
            hints.append(s)
    return dlls, hints

def import_calls(e):
    """set of (DLL, ordinal) that are actually called through a stub"""
    c, base, ts = e.code, e.code_base, e.text_size
    iat, idx = {}, 0
    for dll, ents in e.imports:
        short = dll.split('[')[0].split('.')[0].upper()
        for o in ents:
            iat[ts + 4 * idx] = (short, o); idx += 1
    found = set()
    for i in range(0, ts - 12, 4):
        w = struct.unpack_from('<4I', c, i)
        if w[0] == 0xe59fc004 and w[1] == 0xe59cc000 and w[2] == 0xe12fff1c and (w[3] - base) in iat:
            found.add(iat[w[3] - base])
    return found

def channels(e):
    """RFCOMM channels from 'mov r1,#N ; bl ESOCK_181' (TSockAddr::SetPort)
    and 'mov r3,#N ; bl BTMANCLIENT_62' (TBTServiceSecurity ctor)."""
    c, base, ts = e.code, e.code_base, e.text_size
    iat, idx = {}, 0
    for dll, ents in e.imports:
        short = dll.split('[')[0].split('.')[0].upper()
        for o in ents:
            iat[ts + 4 * idx] = (short, o); idx += 1
    thunks = {}
    for i in range(0, ts - 12, 4):
        w = struct.unpack_from('<4I', c, i)
        if w[0] == 0xe59fc004 and w[1] == 0xe59cc000 and w[2] == 0xe12fff1c and (w[3] - base) in iat:
            thunks[i] = iat[w[3] - base]
    found = set()
    for i in range(0, ts - 4, 4):
        w = struct.unpack_from('<I', c, i)[0]
        if (w & 0xff000000) != 0xeb000000:
            continue
        off = (w & 0xffffff); off = off - (1 << 24) if off & 0x800000 else off
        tgt = i + 8 + 4 * off
        t = thunks.get(tgt)
        if t not in (('ESOCK', 181), ('BTMANCLIENT', 62)):
            continue
        reg = 1 if t[0] == 'ESOCK' else 3
        for back in range(4, 40, 4):
            p = struct.unpack_from('<I', c, i - back)[0]
            if (p & 0x0ffff000) == (0x03a00000 | (reg << 12)) and (p & 0xf00) == 0:
                found.add((t[0], p & 0xff)); break
    return sorted(found)

def analyse(E32, variants, path):
    e = E32(path)
    data = open(path, 'rb').read()
    gc = [o for n, o in e.imports if n.upper().startswith('GAMECOMMS')]
    gc = gc[0] if gc else []
    names = variants.VARIANTS[6][2] if max(gc or [0]) <= 57 else variants.VARIANTS[10][2]
    if gc and max(gc) <= 33 and 11 in gc and 39 not in gc:
        names = variants.VARIANTS[1][2]
    dlls, hints = own_bt(e, data)
    print(f'== {path}  (uid3 {e.uid3:#010x})')
    if not gc:
        print('   does not import GAMECOMMS.DLL'); return
    for o in sorted(gc):
        print(f'   {o:3d}  {names.get(o, ("?", "?"))[1]}')
    bt = [d for d in ('BLUETOOTH', 'BTMANCLIENT', 'SDPAGENT', 'SDPDATABASE', 'ESOCK') if d in dlls]
    print(f'   Bluetooth/socket libraries: {", ".join(bt) or "none"};  strings: {", ".join(hints) or "none"}')
    calls = import_calls(e)
    habits = []
    if 'L2CAP' in hints and ('ESOCK', 87) in calls:
        habits.append('checks the ACL link count before starting ("Bluetooth busy" while any link exists)')
    if ('ESOCK', 124) in calls and ('ESOCK', 176) in calls:
        habits.append('opens RHostResolver and probably renames the phone itself (SetHostName)')
    if 56 in gc:
        habits.append('sets its name through SetLocalDeviceName')
    for h in habits:
        print('   habit: ' + h)
    ch = channels(e)
    if ch:
        print('   RFCOMM channel constants: ' + ', '.join(f'{v} ({k})' for k, v in ch))
    s = set(gc)
    is_v1 = names is variants.VARIANTS[1][2]
    if is_v1:
        verdict = 'GameComms 1.x session API: replacement carries the traffic'
    elif not (s & GROUPS['session']):
        verdict = ('BYPASS: no session/transport functions imported. Multiplayer runs on the '
                   "game's own sockets; a GAMECOMMS.DLL replacement cannot carry it")
    elif s & GROUPS['helpers_list']:
        verdict = 'uses the exported CGameBTDeviceList, which is inert in the replacement'
    elif s & (GROUPS['helpers_disc'] | GROUPS['helpers_res']):
        verdict = 'own host search through the exported discoverer/resolver'
    elif 'BTLinkManager' in hints and 39 in s:
        verdict = 'GameComms transport, but own Bluetooth inquiry (RHostResolver) likely used for host search'
    else:
        verdict = 'standard GameComms 2.x session API'
    print('   verdict: ' + verdict)

if __name__ == '__main__':
    E32, variants = load_repo(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    for arg in sys.argv[1:]:
        paths = [arg] if os.path.isfile(arg) else [os.path.join(r, f) for r, _, fs in os.walk(arg)
                                                   for f in fs if f.lower().endswith(('.app', '.dll', '.exe'))]
        for p in sorted(paths):
            try:
                analyse(E32, variants, p)
            except Exception as ex:  # not an E32 image
                pass
