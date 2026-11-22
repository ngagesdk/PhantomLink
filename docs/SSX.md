# SSX Out of Bounds online service

SSX Out of Bounds (EA, 2004; 6R65.APP, UID 0x101fd402, retail version
041211) is one of the first N-Gage Arena games. It has none of the later
Arena DLLs (ArenaFramework, snapcomm, sc_lib). Its Arena menus (the
statically linked "GSBLIB", Game Services Browser) talk over IPC to
AIRPLAYSERVER.EXE from the game card (Ideaworks3D), which speaks its own
reliable, encrypted UDP protocol with the servers.

Online features:

- Shadow Racing: upload your best run of a track (a shadow, the
  `*_best.ghz` file), see the top times or scores of every track and
  download other players' shadows to race against them.
- Careers: upload your career (completion and earnings), see your own
  summary (My Career) and other players' careers (View Career).
- Fresh Gear: boards, an outfit and cash. The game decides this itself
  from its own records (6R65.APP 0x1007ef30); it only has to reach the
  Arena menu. On the first visit it also pays a 25,000 bonus.
- Accounts: create, log in, change the password, forgotten user name (by
  IMEI) and password (security question).

Everything here comes from static analysis of AIRPLAYSERVER.EXE and
6R65.APP. Status: tested against a simulated game only.

## Setup

1. Build `dll/ssx` with the N-Gage toolchain (part of the `dll` build,
   option `GC_BUILD_SSX`): `out/ssx/abtesrv.dll`. The PhantomLink SIS
   installs it to `C:\System\Libs\abtesrv.dll`; back up any existing DLL
   there before installing. Also put the built DLL where the server finds it:
   `ssx_billing_dll` (default `abtesrv.dll` in the server's working
   directory). The server can update the phone's copy when its MD5 differs.
2. Server: nothing else. `ssx_data` (default `gamecomms-ssx.dat`) keeps
   the uploads. For Arena over GPRS the server listens on UDP 41001
   (`direct_ssx_port`).
3. N-Gage: Arena over BT (or Arena over GPRS) in the PhantomLink app, as
   for the other Arena games. The hosts file entry for
   `discovery.cng.n-gage.com` points the game to the bridge or server;
   the bridges also resolve the name themselves.
4. In the game: N-Gage Arena, Create New User (or log in with an existing
   PhantomLink Arena account).

## How the game connects

1. Discovery: AIRPLAYSERVER.EXE resolves `discovery.cng.n-gage.com` and
   sends UDP to port 41001. After the key exchange it sends message 50
   with the MD5 of `C:\System\Libs\abtesrv.dll` (MD5 of nothing when the
   file is missing).
2. When the MD5 differs from the server's DLL, the server answers 150
   with result 1, the game asks for the file (51) and gets it in 121
   chunks; AIRPLAYSERVER.EXE writes it to `C:\System\Libs\abtesrv.dll`.
   Result 0 means: keep the one you have.
3. The game loads the DLL (RLibrary::Load, ordinal 1) and takes the game
   server's host name and port from it. In the original this was
   EA's/Nokia's "billing DLL"; without one the game says "Invalid
   Billing DLL". PhantomLink's DLL returns `discovery.cng.n-gage.com` and
   41001, so the same server answers.
4. The game connects again (new key exchange) and logs in (0) or creates
   an account (16). After the logon the DLL is asked for an account check
   (slot 0x14), which PhantomLink's passes at once.
5. The game lists object 2000100 (9) and shows the Arena menu.

## Protocol

All datagrams go to UDP port 41001. Integers are little endian. A string
is a u32 length followed by the bytes (no terminator).

### Header

| Offset | Size | Content |
|---|---|---|
| 0 | u32 | sequence number |
| 4 | u8 | message type |
| 5 | u8 | 1: no acknowledgement wanted (acks, logout) |
| 6 | u16 | 0x00ff (0 in acks); bit 15: body encrypted |

Every message with byte 5 = 0 is acknowledged with type 0xfa, body u32
sequence. Unacknowledged messages are resent after 2 seconds. The game
remembers every sequence number it has seen for 90 seconds and drops a
message with a known one, acknowledgements included; the server
therefore numbers its acks too. The game numbers each connection from the
start, so the server starts a new duplicate list with each key request.

### Key exchange

| Message | Direction | Body |
|---|---|---|
| 18 | game | empty, not encrypted |
| 133 | server | string modulus, string exponent (decimal) |
| 19 | game | string: RSA-encrypted 16-byte session key |
| 134 | server | u8 1 |

The game encrypts the key with CRSAPublicKey::PublicEncryptL (cryptalg
default: PKCS#1 v1.5). The server makes a new 1024-bit key pair at its
first key exchange; the game accepts moduli up to 2048 bits.

### Encrypted bodies

Cipher: TCipher3DESCBC with the key halves K1 = bytes 0..7, K2 = bytes
8..15 and an empty third key, which cryptalg replaces by K1: two-key
triple DES (EDE), CBC, no padding object. Body:

| Field | Content |
|---|---|
| string | IV (8 bytes, random per message) |
| u32 | plain length L |
| bytes | cipher text |

The sender encrypts: plain, the first 8 bytes of a hash of the plain
text, zero bytes up to a multiple of 8, and one block of eight 0x08
bytes. The receiver decrypts all but the last block and accepts only
plain lengths n with L + 8 <= n < L + 16 and a matching hash. The hash
comes from hash.dll ordinal 25; whether that is MD5 or SHA-1 is not known
(no ARM import library of hash.dll was found), so the server takes the
one that matches the game's first encrypted message.

### Messages

Game to server:

| Type | Name | Body |
|---|---|---|
| 0 | logon | account header, user, password, IMEI, phone, u8 |
| 1 | logout | empty |
| 3 | download object | u32 object id, string |
| 4 | download file | string, string (not used by SSX) |
| 6 | upload | u16 code, u32 size, string description, u32 a, u32 b |
| 7 | upload by name | string name, u32 size, string, u32, u32 (not used) |
| 8 | upload chunk | u32 first, u32 last (inclusive), u32 length, bytes |
| 9 | list objects | u32 parent id, string |
| 10, 11 | file list, message list | (not used by SSX) |
| 12 | time | u32, u32 (the phone's clock) |
| 13 | change password | account header, user, old, new, question, answer, IMEI, phone, u8 |
| 14 | question | account header, user, IMEI, phone, u8 |
| 15 | password | account header, user, answer, IMEI, phone, u8 |
| 16 | create user | account header, user, password, question, answer, IMEI, phone, u8, string, u8 |
| 17 | user name | account header (without the game id), IMEI, phone, u8 |
| 50 | discovery | u16 2, string DLL MD5, string DLL name, u16 39, u16 10, IMEI, phone |
| 51 | discovery file | empty |

The account header is u16 3, four u16 version numbers, the string with
the DLL MD5 and the u32 game id (30003).

Server to game:

| Type | Name | Body |
|---|---|---|
| 120 | logon / create result | u8 result, u8, u32, u8, string message |
| 121 | file chunk | u32 total, u32 first, u32 last, u32 length, bytes, u32 object id (object downloads only) |
| 122 | upload ready | empty |
| 123 | upload done | empty |
| 124 | object list | u8 count, u8 packet, u8 packets, u32 parent id, entries |
| 125, 126 | file list, message list | u8 count, u8 packet, u8 packets |
| 127 | time | the two u32 of message 12 |
| 129 | change password result | u8 |
| 130 | question | u8 result, string question, u8 |
| 131 | password | u8 result, u8, string password |
| 132 | user name | u8 result, string user, string message |
| 150 | discovery result | u8 (0 keep the DLL, 1 a new one follows), string |

Logon results: 0 ok, 1 authentication failed, 2 user not found, 3 user
name exists, 4 cannot change user name, 5 protocol too old, 254 protocol
too new, 6 user barred, 7 age check failed, 8 unknown network error.
Results of 130/131: 2 user not found, 5 wrong answer (131).

List entry: u32 id, u16 type, u16 upload type, u32 value 1, u32 value 2,
u32 value 3, u32, string name (the game keeps 20 bytes), string. Type 0
or 1 is a folder; an entry of another type with upload type 0 is
downloaded when chosen. The game keeps the packets of a list by packet
number and drops lists of another parent.

### Menu tree

The game knows these ids and draws their entries itself (6R65.APP
0x1010d4cc and 0x1007634c):

| Id | Entry | Server |
|---|---|---|
| 2000100 | Arena menu | the entries below |
| 2000109 | Shadow Racing | Peak 1, Peak 2, Peak 3 |
| 2000110, 2000120, 2000130 | Peak 1, 2, 3 | track folders 2000111..116, 2000121..125, 2000131..134 |
| 2000111..2000149 | a track | its shadows, best first |
| 2000150 | My Career | the player's own career |
| 2000160 | View Career | all careers |
| 2000180 | Upload Shadows | (screen of the game) |
| 2000190 | Upload Career | (the game uploads) |
| 2000170 | Fresh Gear | (screen of the game) |
| 2000195 | Tips and Tricks | (screen of the game) |
| 2000140 | Disconnect | (the game logs out) |

Tracks 0..14: Snow Jam, Metro City, R&B, The Junction, Happiness,
Happiness Jam (Peak 1), Ruthless Ridge, Intimidator, Style Mile,
Ruthless, Ruthless Jam (Peak 2), Gravitude, Kick Doubt, The Throne,
Throne Jam (Peak 3). Tracks 2, 3, 5, 8, 10, 12 and 14 are scored by
points (higher is better), the others by time in milliseconds (lower is
better).

A shadow entry has type 10122 + track, value 1 the time or score, value
2 the rank, value 3 the upload date (seconds since 1970). A career entry
has type 1001, value 1 the completion, value 2 the earnings, value 3 the
upload date. Downloads go to `d_<track>_temp.ghz` and `careersave.dat` in
the game's folder.

### Uploads

| Code | What | a | b | File |
|---|---|---|---|---|
| 10121 | career | completion | earnings | `tmpscore.dat`: the 0xdc-byte career record |
| 10122 | shadow | time (ms) or score | track | `<track>_best.ghz`, header u16 0x3e9, track at offset 4 |

The game sends all chunks (at most 1255 bytes each, datagrams of about
1300 bytes) right after 122. A new upload replaces the player's old one
(career, or shadow of the same track).

## Implementation

| Part | Files | Status |
|---|---|---|
| Protocol, accounts, tree, uploads, downloads, data file | `server/src/gcs_ssx.c`, `gcs_ssx.h` | written, tested against a simulated game |
| MD5, SHA-1, triple DES, RSA | `server/src/gcs_ssxcrypt.c` | written, test vectors |
| UDP port 41001 for bridges and direct clients | `gcs_native.c`, `gcs_direct.c`, `gcs_main.c` (`direct_ssx_port`) | written, tested |
| Large datagrams through the bridges (UDP_PART) | `protocol/src/gc_net.c`, `firmware/src/bridge_core.c` | written, tested |
| Billing DLL | `dll/ssx/AbteSrv.cpp` | written, not built here |
| Hosts entry | `app/src/ArenaSetup.cpp` | written, not built here |
| Tests | `tests/server/test_ssx.c` (`server_ssx`) | |

Data file (`ssx_data`), tab separated lines: `N` next object id; `S` id,
track, value, date, user, file in hex; `C` id, completion, earnings,
date, user, file in hex.

Accounts are the shared PhantomLink accounts (docs/ARENA.md): an account
made in SSX works in the other Arena games and the other way round.
Logons do not create accounts (the game has Create New User for that).
The security question and a SHA-1 of the lower case answer are kept as
account fields `ssx_question` and `ssx_answer`. Only a hash of the
password is kept, so a forgotten password cannot be shown: after a right
answer the server sets a new random password and the game shows that.

## Assumptions to check on hardware

- The layout of the billing DLL's interface (vtable slots 0x08..0x34,
  host name as TBuf16<72>) and that the game accepts the completion codes
  0x96, 0x98, 0x9a.
- MD5 or SHA-1 as the check hash (the server handles both).
- The meaning of the u32 and u8 in the logon answer (sent as 0).
- Whether the hosts file reaches AIRPLAYSERVER.EXE's resolver (the
  bridges resolve the name either way).
- Upload chunk datagrams through the bridge (UDP_PART) and through a
  carrier NAT for GPRS.

## Known limits

- The ESP32 bridge asks the N-Gage for an MTU of 1100 bytes, and its IP
  stack drops fragments. SSX's upload chunks (datagrams of about 1300
  bytes) then arrive in IP fragments and are lost: shadow and career
  uploads need the PC or Android bridge (MTU 1500) or Arena over GPRS.
  Everything else works with every bridge.
