# N-Gage Arena support (work in progress)

Some N-Gage games, such as Pocket Kingdom: Own the World (UID 0x101fd3dd) and
Rifts: Promise of Power (game class 8053), do not use GameComms for their
online modes. They ship the N-Gage Arena client stack (Nokia's
version of SEGA's SNAP middleware) in their own `E:\System\Libs` and went
online over GPRS. PhantomLink replaces the three network clients of that
stack with plug-ins that use the PhantomLink Bluetooth bridge. The server
emulates the Arena back ends.

```
game ─ arenaframework.dll ─┬─ arenabase.dll ─┬─ SnapComm.dll ───────┐
                           │                 └─ HttpDllRequest.dll ─┤  replaced: PhantomLink plug-ins
                           └─ sc_lib.dll (SegaChat/Jabber) ─────────┘   + plarena.dll (shared link)
                                                                        │ Bluetooth SPP
                                                         bridge (ESP32 / PC / Android, unchanged)
                                                                        │ TCP
                                                         gamecommsd + gcs_arena.c
```

## Retail games: PPP over Bluetooth (no DLLs replaced)

On a retail cartridge everything the game ships is read only, and its own
`E:\System\Libs` is searched before `C:`. `arenabase.dll` even loads
SnapComm.dll and sc_lib.dll from its own folder. The plug-ins below
therefore only work on a writable copy of the card. For retail cards the
original Arena stack stays in place and only its network path changes:

```
game + original Arena DLLs (card)
  │ TCP/IP through the access point "PhantomLink"
CSD agent, empty number = no dialling, PPP over BTCOMM::0          (phone settings on C:)
  │ Bluetooth serial port
bridge (PC, ESP32 or Android): first byte 0x7E → PPP mode          (bridge_core.c)
  │ gc_ppp: LCP/IPCP, N-Gage 10.64.0.2, bridge 10.64.0.1
  │ gc_ip:  DNS answered here (Arena names → 10.64.0.1), TCP/UDP terminated
  │ GCN tunnel messages (gc_net.h) over the normal link
gamecommsd + gcs_native.c: Community SOAP and pages (TCP 80), SegaChat (TCP 5222),
                           SNAP (UDP 9090, gcs_snap.c) + game servers (Rifts: gcs_rifts.c)
```

No pppd, no root and no Python script are needed any more: every bridge
carries its own PPP peer and IPv4 end point (`protocol/src/gc_ppp.c`,
`protocol/src/gc_ip.c`), and the server runs the Arena services. The
bridge registers with the server for the N-Gage (API family 3, the
N-Gage's Bluetooth address as device id) and tunnels each TCP connection
and UDP datagram:

| GCN message | Direction | Content |
|---|---|---|
| `TCP_OPEN` 0xA0 | bridge → server | id, address and port the game connected to, its own port |
| `TCP_DATA` 0xA1 | both | id, bytes (server → bridge at most 512 per message) |
| `TCP_CLOSE` 0xA2 | both | id, FIN or reset |
| `UDP` 0xA3 | both | server side address and port, the game's port, bytes |

Connection ids carry a generation byte, so late messages for a closed
connection never reach its successor. The bridge acknowledges TCP data from
the N-Gage only when the server link can take it, and takes data from the
server only when the TCP send buffer has room: the N-Gage's TCP and the
link's own flow control do the rest. When the server link comes back
fresh, the bridge registers again and resets the open connections.

Steps:

1. On the N-Gage: Settings > Connection > Access points > new access
   point **PhantomLink**, data bearer GSM data, any phone number.
2. PhantomLink app: choose the bridge, then **Arena over Bluetooth**. It
   changes that access point to a direct connection (empty number, no login
   script, addresses from the server), switches the data modem to
   `BTCOMM::0` / `BTCOMM`, points `BTCOMM::0` at the bridge
   (`CBTRegistry::SetDefaultCommPort`) and writes the hosts file. On
   Symbian 6.1 the modem record is shared by all data call access points,
   so the old values are saved; **Arena off** puts them back and removes
   the hosts entries.
3. Bridge: nothing to set up; PPP mode is on by default (`ppp=1` in
   gcbridge.ini, `ppp_local` / `ppp_remote` change the addresses).
4. Server: `gamecommsd`; the accounts and their profile data go to
   `arena_accounts` (default `gamecomms-arena.txt`), the Rifts battles to
   `rifts_data` (default `gamecomms-rifts.dat`), both in its working
   directory.
5. Start the game, choose N-Gage Arena and pick the PhantomLink access
   point when asked.

What the server answers (shapes taken from the parsers in the Arena DLLs,
see the reverse engineering notes below):

- Community SOAP: `checkImei`, `authenticateUser2` / `createUser` (true plus
  the user name as the first `xsd:string`, which the client uses for Jabber
  and in URLs, and a random session cookie; false for a refused login, see
  "Accounts" below), the `view*` and `edit*` account, display, alert,
  favourites and quote operations with the data kept per user. `viewFavorites`
  returns six plain `<item>` elements in client order: game, game types, music,
  food, hobbies and zodiac. The client sends game types as `favMusic` and music
  as `editFavorites` in its edit request; these wire names are also used in the
  account store. Also supported are `changePassword` (checks `oldPassword`
  when the client sends it), the security question, and `errorCode 0` for every
  other operation (1 when the request does not belong to a logged in user).
- Device pages (news, support, lists): served from the folder `www_root`
  when set, `www_root/<game class>/n-gage/...` before `www_root/n-gage/...`
  (game class of the user's SNAP login, e.g. 8053 for Rifts); without a
  file `*_x.jsp` lists are empty lists, other `.jsp` pages and
  `/servlets/*` a short black page. The profile view is generated from the
  requested account instead of a file: it shows the username, names enabled
  in display options, favourites and quote, but not email or credentials.
  An invalid or unknown username returns 404. The addresses are
  ArenaFramework's, the same in every game; example pages and the list of
  addresses in `server/deploy/www`.
- SegaChat: stream, `jabber:iq:auth` get/set (the password is checked, see
  below), empty roster, an empty result for every other iq; presence and
  messages are not routed yet.
- SNAP (UDP 9090): login, lobbies, game rooms, data between room members
  and the game servers behind the rooms, see "SNAP" below. Datagrams
  without the SNAP trailer are logged as hex.

### Accounts (shared by all games)

All Arena games and back ends use one account store
(`server/src/gcs_accounts.c`, file `arena_accounts`):

- The first `authenticateUser2` (or `createUser`) with an unknown user
  name creates the account and binds user name, password and the IMEI the
  N-Gage sends.
- Later logins need the same password and the same N-Gage. One N-Gage can
  own only one account. A refused login answers `false`.
- SegaChat logins send the SHA-1 of the password (40 hex digits, seen on
  hardware); they are checked against the same account and never create
  one. A wrong password gets `<error code='401'>`.
- A SNAP login is accepted when the account exists and the same N-Gage
  (bridge peer) has logged in to the Community service with that name;
  the SNAP login itself carries no password the server could check.
- Passwords are stored as SHA-1 (`pwsha1`), not in plain text. Files of
  the first version (plain `password` lines) are converted when loaded.
- To move an account to another N-Gage, stop the server and remove the
  account's `imei` line; the next login binds the new IMEI.
- The session cookie is random and kept by the server; without a valid
  cookie a request counts for the user last logged in on that N-Gage.

`tools/arena/arena_native.py` does the same on a PC next to pppd. It is
kept for experiments but no longer needed.

Tests: `protocol/tests/test_ppp.c`, `protocol/tests/test_ip.c`,
`tests/net/ip_tun.c` (gc_ip against the Linux TCP/IP stack),
`tests/sim/test_ppp_e2e.c` (bridge core + server in one process) and
`tests/net/run_ppp_test.py` (gamecommsd + gcbridge + `pppsim`, which plays
the N-Gage with the Linux TCP/IP stack behind a TUN interface; needs root).

### Hosts file

Symbian OS 6.1 to 9.0 read `C:\System\Data\hosts` (9.1 and later:
`C:\private\10000882\hosts`). The format is one `address name` pair per
line. Symbian's FAQ-1282 (written for 6.1 to 8.1) says to restart the
phone after changing it. The file is on C:, so it works with retail
cards. The PhantomLink app maps `arena.n-gage.com`,
`im.prod.pk.nokia.sf.yav4.com`, `snap.prod.pk.nokia.sf.yav4.com` (Pocket
Kingdom), `im01.rifts.bb.sf.yav4.com`, `bs01.rifts.bb.sf.yav4.com` (Rifts)
and `ngage-auth` to 10.64.0.1 (Bluetooth) or to the server's public address
(GPRS) and keeps all other lines (its own lines carry the tag
`# PhantomLink Arena`). Because it is unverified whether the N-Gage
resolver uses the file for these names, the bridge also offers its own
DNS server through IPCP and answers it. Either path is enough on its own
over Bluetooth; over GPRS the hosts file is the only one (see below).

## Retail games: Arena over GPRS (direct, no bridge)

Where a GPRS network still works, the N-Gage can reach the server on its
own, the way the original Arena was used. Nothing on the card or in the
phone's network settings changes; only the Arena host names are pointed at
the PhantomLink server:

```
game + original Arena DLLs (card)
  │ TCP/IP through the operator's packet data access point
  │ C:\System\Data\hosts: Arena names → the server's public IPv4 address
operator network, internet
  │
gamecommsd --direct=1: TCP 80 and 5222, UDP 9090 (gcs_direct.c)
  │ the same GCN messages a bridge would send, one peer per client address
gcs_native.c: Community SOAP and pages, SegaChat, SNAP + game servers
```

GPRS and Bluetooth players use the same accounts, lobbies and game
servers.

Steps:

1. Server: run `gamecommsd` with `direct = 1` on a machine with a public
   IPv4 address (or a router that forwards TCP 80, TCP 5222 and UDP 9090
   to it). The game always uses these three ports; `direct_http_port`,
   `direct_xmpp_port` and `direct_snap_port` only change where the server
   listens, for a port forward such as 80 → 8080 (ports below 1024 need
   privileges, see `server/deploy/gamecommsd.service`). `direct_bind`
   (default `0.0.0.0`) limits it to one interface. These listeners speak
   only the Arena protocols in plain text, like the original Arena; the
   bridge port keeps its own rules (loopback, access key).
2. On the N-Gage: a packet data access point for the operator (Settings >
   Connection > Access points, data bearer Packet data, the operator's
   APN). Most phones have one from the operator's settings message.
3. PhantomLink app: Settings > **Arena server**: the server's IPv4 address,
   or a host name (looked up when the next step runs, which opens a data
   connection; run it again if the address changes).
4. PhantomLink app: **Arena over GPRS**. It lists the packet data access
   points, writes the hosts file and asks for a restart.
5. Restart the N-Gage, then choose **Arena over GPRS** once more. With the
   entries already in place it looks up `arena.n-gage.com` and reports
   "Arena names: OK" when the answer is the server. **Do not log in
   before it does**: without the hosts file the game would send the
   account name and password to whoever holds the old Arena domains today.
6. Start the game, choose N-Gage Arena and pick the operator's access point
   when asked. **Arena off** removes the hosts entries again (and restores
   the modem if Arena over BT changed it).

How the server tells clients apart: each client address is one peer of
`gcs_native`. Several N-Gages behind the same carrier NAT share an address,
so the server keeps every Community login per peer (not only the last one);
the session cookie picks the user for SOAP and page requests, and a SNAP
login is accepted for any account that logged in to the Community service
from the same address. Datagrams are only taken from addresses with a TCP
connection open or one in the last 10 minutes (`GCSD_IDLE_MS`), so spoofed
UDP sources are ignored. Silent TCP connections are closed after
`direct_idle_ms` (default 2 minutes).

Not verified on hardware: whether the N-Gage's resolver uses the hosts file
for these names (the check in step 5 tells), and how carriers' NATs treat
the SNAP datagrams over longer games (the client sends regularly, which
keeps a NAT mapping open). Tests: `tests/server/test_direct.c` (in
process: logins, two N-Gages behind one address, SNAP, unknown sources,
idle addresses) and `tests/net/run_direct_test.py` (gamecommsd with real
sockets).

## Parts

| Part | Files | State |
|---|---|---|
| GCA messages (types 0x80..0x9F) | `protocol/include/gc_arena.h`, `protocol/src/gc_arena.c` | done, tested |
| Server Arena module | `server/src/gcs_arena.c` | done, tested (generic) |
| Shared link core (portable) | `dll/arena/core/pla_core.c` | done, tested against the server core |
| plarena.dll (Bluetooth, log) | `dll/arena/symbian/PlArena.cpp` | written, **not built yet** |
| SnapComm.dll | `dll/arena/symbian/PlSnapComm.cpp` | logs all slots, forwards login |
| sc_lib.dll | `dll/arena/symbian/PlScLib.cpp` | logs all slots |
| HttpDllRequest.dll | `dll/arena/symbian/PlHttpRequest.cpp` | logs all slots |
| Retail: access point + hosts | `app/src/ArenaSetup.cpp` | done, works on an N-Gage |
| Retail over GPRS: hosts + check (app), direct clients (server) | `app/src/ArenaSetup.cpp`, `server/src/gcs_direct.c` | written; server tested, app syntax-checked only, **not yet on hardware** |
| Retail: PPP + IPv4 in every bridge | `protocol/src/gc_ppp.c`, `gc_ip.c`, `gc_net.c`, `firmware/src/bridge_core.c` | done, tested (PC bridge with Linux as the N-Gage) |
| Retail: native back ends | `server/src/gcs_native.c` | SOAP/pages/Jabber login answered |
| Retail: shared accounts | `server/src/gcs_accounts.c` | done, tested |
| Retail: SNAP lobby/room service | `server/src/gcs_snap.c` | written from the client DLLs, tested against simulated games, **not yet on hardware** |
| Retail: Rifts game server | `server/src/gcs_rifts.c`, `server/src/gcs_rcodec.c` | written, tested against simulated games, **not yet on hardware** |

The DLLs build only with `-DGC_BUILD_ARENA=ON` (off by default until the
build has been verified with the N-Gage toolchain). Copy `plarena.dll`,
`snapcomm.dll`, `sc_lib.dll` and `httpdllrequest.dll` over the game's
copies in `E:\System\Libs`. Every call is logged to
`C:\System\Data\PlArena.log` with its arguments and the caller's return
address. That log is the next step for identifying the remaining slots.

## Not tied to one game

- The server keeps users, lobbies and rooms per **game class id** (sent
  in LOGIN). Every Arena title gets login, lobbies/rooms created on
  demand by name, member events, room status values, room and private
  data relay, chat, presence and the Community web service without game
  specific code.
- Game specific server logic plugs in through `gcs_arena_add_game()`.
  Hooks: login, data, service requests, logout; a module answers with
  `gcs_arena_send_to()`. Pocket Kingdom's persistent world will be such
  a module.
- The plug-ins replace DLLs that are shared by all Arena games. Other
  titles may ship other versions of the stack with different vtables,
  as with the GameComms variants. Check a game's DLLs (md5, export count,
  vtable size) before assuming the layouts below.

## SNAP (lobby and game room service)

Findings from snapcomm.dll ("kkClient Ver 2.1 Build: Aug 4 2005", ARM
code) and ArenaFramework.dll 2.01.082 of Rifts: Promise of Power. The
server side is `server/src/gcs_snap.c`.

### Transport

One UDP socket. The login goes to port **9090** of the server named
`GameServerFQDN` in `arena.amr`; the login answer can move the client to
another port (the server keeps 9090). The client only accepts datagrams
from the address and port it sends to. A datagram is one or more
messages followed by the trailer `BA 47 66 xx` (the client sends `11`
and checks only the first three bytes); it holds at most 896 bytes.

Message header, 16 bytes, big endian:

| Offset | Size | Content |
|---|---|---|
| 0 | 2 | flags and length: 0x8000 reliable, 0x4000 the ack field is valid, 0x2000 single packet (`KK_SINGLE_PACKET_DEF` in the game server's kkGSApi.h; without it byte 2 is the `PacketSeq` of a series, counting down to 0, and ArenaFramework takes a list as complete only at 0; both sides set it on every message), 0x1000 target is the lobby (else the game room), 0x0800 another message follows in the same datagram (the client numbers only the first reliable message; reliable messages packed behind it carry sequence number 0, share its acknowledgement and are processed with it), 0x0400 game settings data; bits 0..9 the length including the header |
| 2 | 1 | command sequence of the request; answers echo it (ArenaFramework matches attribute changes by it) |
| 3 | 1 | opcode |
| 4 | 4 | user id (from the login answer) |
| 8 | 4 | sequence number: reliable messages count from 0 after the login, unreliable ones are dropped when older than the last. snapcomm.dll numbers one reliable message per datagram; others carry 0 (packed behind it, or sent alone, like Rifts' room attributes ARNC and ARND): the server processes those once and tells repeats by opcode and command sequence |
| 12 | 4 | acknowledged reliable sequence number |

Every reliable message is acknowledged on its own (the client sends a
16 byte message with flags 0x6010, opcode 0 and the sequence number in
the ack field) and resent until then (`MAX_RETRIES` 17, then the
connection counts as lost). Reliable messages are delivered in order.

### Opcodes

Requests (client → server):

| Op | Flags | Payload | Meaning |
|---|---|---|---|
| 44 | 0x3000 | user name [40], [60] text, game class (BE) at 100, ..., TLV `LC`/`MR` (latency, retries) | login |
| 2 | 0xb000 | - | logout |
| 3 | 0xb000 | name [16] | lobby by name |
| 14 | 0xb000 | - | lobby list |
| 4 | 0xb000 | name [16], max users, password [16], two words | create game room |
| 5 | 0xb000/0xa000 | id | (not identified) |
| 6 | 0xb000 | lobby id (or name [16] + id) | enter lobby |
| 6 | 0xa000 | room id, password [16], word | enter game room |
| 7 | 0xb000/0xa000 | - | leave lobby / room |
| 8 | 0xa000 | attribute list | change room attributes |
| 9, 10, 12, 13, 67 | | | attributes, user lists (not identified in detail) |
| 11 | 0xb000 | lobby id | search game rooms |
| 71 | 0xb000 | - | lobbies of the login's game class (Rifts sends it when the player creates a challenge) |
| 15 | 0xa400 / 0x2000 | data | game data to the room, reliable / unreliable |
| 16 | 0xa400 / 0x2000 | count, user ids, data | game data to some users |
| 17 | 0xa000 | data | game data to the room's **game server** (`SendGameDataToServer`, CArenaFramework vtable slot 0x3c) |
| 18, 19, 20, 50, 37, 70..75 | | | not used by the Rifts flow |

Answers and notifications (server → client):

| Op | Payload | Meaning |
|---|---|---|
| 41 | port (0: keep), -, user id | login accepted |
| 39 | request op, reason | request failed (op 39 with request op 1 = login refused) |
| 40 | request op, value, ... | request succeeded; the 0x1000 flag tells lobby from room answers (op 6: "Entered lobby" / "Entered room", value = id) |
| 3 | name [16], lobby id | a lobby (ArenaFramework builds its lobby list from these) |
| 4 | room entry | the created room |
| 11 | header {parent, total, count} + count × room entry (name [16], id, users, max, status, lobby) | room search result |
| 14 | header + count × {name [16], id, members, max} | lobby list |
| 71 | header {game class, total, count} + count × {name [16], users, -, max, status, lobby id} | lobbies of the game class; no op 40 follows (ArenaFramework would start a new, empty list with it) |
| 10 | header + count × {name [16], user id, property size} | user list |
| 6 | {name [16], user id, property size} | a user entered the room |
| 7 | user id | a user left the room |
| 15 | data | game data (also what the game server sends; arrives as `GameDataReceived`) |

### Flow of Rifts (game.rsc)

Rifts sets `GameClassId` 8053, `BypassCommunity`, `LobbyToJoin` "S79",
`RoomToJoin` "BYPASS", `CreateRoomToJoin` 1 and `ReturnContextOnRoomEntry`
1. Seen on hardware: Community login (SOAP) → SNAP login → SegaChat login;
creating a challenge then sends op 71 (lobbies of the game class), and
ArenaFramework looks for "S79" in that list. Then: op 13 + enter lobby
(op 6, packed) → room search (op 11) → op 13 + create room "BYPASS<user id>"
(op 4, packed) →
room attributes (op 8: ARNA, ARNB, ARNC, ARND; creating the room counts as entering it: the server makes the creator a member, there is no join; ArenaFramework goes on only when all four are confirmed) → room status (op 8 STAT 0x90000000) → `EGameStart` → game data to
the game server (op 17), answers as op 15. The server creates lobbies on
demand, gives every created room a game server when one is registered for
the login's game class, and closes empty rooms.

Unverified on hardware: whether ArenaFramework needs more after the room
entry (for example a room status notification) before `EGameStart`, and
whether the login answer is enough without the encrypted challenge
(op 64 → op 65, done with sustandard.dll) the client also understands.
The server log at level 3 shows every message in both directions.

## Rifts: Promise of Power

Rifts' Arena mode is played "by e-mail": a player creates a challenge,
others find and accept it, and then each uploads a turn when it suits
them; the server keeps everything. The game talks to its game server
through the SNAP room (op 17 in, op 15 out). The server side is
`server/src/gcs_rifts.c`, an independent implementation written from the
behaviour described here.

### Game messages

Little endian, ARM layout (`MessageBase` is id u8, relay u8, source s8,
padding):

| Id | Message | Layout |
|---|---|---|
| 4 | MSG_SERVER | object id i32 @4, action type u8 @8, flags u8 @9, offset i32 @12, length i32 @16, total i32 @20, data [≤ 384] @24 |
| 5 | MSG_SERVER_CANCEL | object id @4, reason @8 |
| 25 | MSG_FLOW_EXTEND_CREDIT | credit @4 |
| 26 | MSG_PING | answered with MSG_PING |

An action is identified by its object id (the client's pointer) and
user. Its request stream comes in MSG_SERVER chunks at offsets that are
multiples of 384; the answer goes back the same way with the same object
id (one message with total 0 when the action has no answer data). A
failed action is answered with MSG_SERVER_CANCEL. Flow control: both
sides start with 8192 bytes of credit; the server sends
MSG_FLOW_EXTEND_CREDIT (+4096) when the client has used half of its
credit, and sends answer chunks only while the client's credit lasts.

### Streams

`u32 size | u32 CRC-32 | zlib | pad` encrypted with Blowfish in CBC mode
(zero IV, 32 bit words little endian) and the 8 byte key
`2c de cb 4a a5 6a 35 07`. The padding fills the zlib data to a multiple
of 8; its last byte is the pad length (8 is written as 8 and read as 0).
The CRC covers the zlib data with the padding. Inside, integers are
written as raw little endian values (`int` 4, `int64` 8, `bool`/`byte` 1,
`time_t` 4) and strings as i32 length plus bytes.
The server writes zlib streams with stored blocks (any inflater reads
them) and has its own inflater (`server/src/gcs_rcodec.c`).

Rows: battle (164 bytes: id i64, 15 ints from kind to friends-only, start
time, round, state, team initiative, seed, next turn device, turn end,
turn end from start, host [32], name [32]); battle player (68 bytes:
battle, chess clock i64, team, power, device, finished, dropped,
user [32]); character (battle i64, id i64, resource, team, device,
user [32], name [32], 265 stats, then an attribute list: count +
{battle i64, id i64, kind, value}).

### Actions

| Type | Action | Request | Answer |
|---|---|---|---|
| 0 | download turns | battle, turn | count, {length, message} after that turn |
| 1 | turn count | battle, turn | count (+ device of the last end-turn message) |
| 2 | upload turns | battle, turn, my device, count, {length, message} | bool |
| 3 | upload characters | battle, team, count, characters, save slot, power | character ids |
| 4 | create challenge | battle row + upload | battle id, character ids |
| 5 | server config | power rank version, save version, protocol | {2, 0, version}, power rank version, versions (echoed), class and item power tables when the version differs |
| 6 | name validation | string | int (0 = offensive) |
| 7 | find challenge | paging data, min/max power, kind (-1 any), players, friends only, ranked only, name [16], max rows, user | count, battles, their players, 4, player count |
| 8 | accept challenge | upload | result (0 waiting, 1 last player: the battle starts, 2 full, 3 error, 4 gone, 5 team taken), device, seed, character ids |
| 9 | challenge status | user, count, {battle, turn, save slot} | count, {state, turns available, next device} |
| 11 | download characters | battle, user | count, the other players' characters |
| 12 | end game | battle, user, wins, losses, points | - |
| 13 | game upkeep | update flag; battle row, or battle, next device, my device + battle, turn | (no update) battle, server time, game over, dropped devices [8] + names, then the download turns answer |
| 14 | update friends | user, count, names | - |

Battle states: 0 waiting for players, 1 challenge deleted, 2 in progress,
3 out of time, 4 ended. Save slots: when a user reports its games
(challenge status), games in slots it no longer reports are given up
(host: challenge deleted, other players: withdrawn, running game: marked
as dropped).

## Reverse engineering notes (Pocket Kingdom retail, SnapComm "Client Ver 2.1 Build: Oct 20 2004")

All Arena DLLs are Thumb code; `tools/re/e32dis.py` disassembles ARM only.

### Loading

| DLL | Loaded by | How |
|---|---|---|
| SnapComm.dll | arenabase | RLibrary::Load from its own `\System\Libs`, UID3 0x101fde38, ordinal 1 = NewLC (arenabase pops) |
| sc_lib.dll | arenabase (dynamic, UID3 0x101fde44), arenaframework + cafmjabber (import ordinal 3) | 1 NewLC (calls slot 0x0c), 2 returns 0, 3 NewL |
| HttpDllRequest.dll | arenaframework, arenabase, cafmlogin: load by name (fallback HttpRequest.dll), ordinal 1 | non-leaving new, returns the object |

### SnapComm object (0x44 bytes, vtable 38 slots)

Each slot calls a C function `snap_xxx(handle, ...)`. The C function
builds a request with a class/flags word (0x8000/0xa000/0xa400/0xb000),
an opcode and a sequence number, sends it (fn 0x895c) and registers the
response callback id (fn 0x2360). It returns the request sequence number.

- 0x08 deleting destructor
- 0x0c init (server address, port, ...), called by NewL(a1..a4)
- 0x10 login: four C strings (r1, r2, r3, sp0), server TInetAddr, a value, three ints, observer. Returns 0 on success.
- 0x14..0x8c requests; arenabase has one wrapper per slot (0x1020..0x19bc), each followed by slot 0x9c. Opcodes seen: 0x07 (slot 0x38), 0x0e (0x4c), 0x47 (0x50), 0x12 (0x74), 0x13 (0x78), 0x11 (0x88), 0x14 (0x8c), 0x0c (0x64), 0x0f (0x6c, 0x80), 0x32 (0x7c)
- 0x90 logout/disconnect, 0x94 clear callbacks, 0x98 SetCallback(id, observer), 0x9c last error
- Callbacks: a table of observer **objects** at handle+0x554, indexed by id. Each id calls a different virtual slot of the observer with (header, parsed struct). arenabase registers ids 0..20 during login (0x796..0xae2).
- Transport of the original: one UDP socket (its own BSD socket layer over ESOCK), default port apparently 8080.

### sc_lib client (vtable 26 slots)

| Slot | SegaChat request |
|---|---|
| 0x08 | destructor |
| 0x0c | ConstructL |
| 0x10 | connect / login |
| 0x14 | startup, then segachat_send_event |
| 0x18, 0x1c, 0x40 | segachat_send_event (event_type, game_class_id, match_id, snap_name, client_time); probably reporter/match results |
| 0x24 | get roster |
| 0x28 | message (chat, groupchat, headline) |
| 0x2c | presence |
| 0x30 | buddy accept/reject |
| 0x38, 0x3c | roster item, remove |
| 0x44 | segachat_retrieve_req (rankings) |
| 0x48 | browse jid |
| 0x4c..0x60 | offline messages: count, headers, fetch, purge |
| 0x64 | buddy request (subscribe) |
| 0x68 | data message |
| 0x6c | service request |

### HttpDllRequest (vtable 21 slots)

The root class declares 7 virtual functions before its destructor (slot
0x24). Slot 0x08 opens the RHTTPSession, 0x10/0x14/0x1c set headers, 0x28/0x2c
take the body, 0x34/0x38 submit, 0x30 cancels, 0x20 closes (inferred from
the HTTP/STRINGPOOL calls). The SOAP envelope is built by arenabase with
httpinterface.dll helpers (SOAPAction, Content-Type text/xml).

## What the game probably needs from the server

From the binaries:

- SOAP Community service: `authenticateUser2` (username, password),
  `createUser` (firstName, lastName, email, dateOfBirth, question,
  answer), `checkImei` (?IMEI=). gcs_arena answers all three with
  `true`; the real return types are unverified.
- Rankings: boards `player_skills`, `highscores`, periodicity
  daily..alltime, through the Jabber retrieval/reporter services.
- SNAP: lobbies ("Scrimmage" is in game.rsc), rooms, matchmaking modes
  freestyle/ranked/filtered/challenge. The game enters a room named
  "Auction" by name.

From the manual:

- Arena mode is unlocked by the "Online Crest" from offline play.
  Online, players "challenge other players to battles and carry out
  auctions". The game box had a separate "N-Gage Arena Quick Start Guide
  for Pocket Kingdom" insert with more detail; worth finding.
- Battles run automatically from the party settings until one side is
  wiped out or the timer ends. Attacking another player therefore
  probably does not need that player online: the server likely stores
  each kingdom (defending parties, battlefield conditions) and hands it
  to the attacker. Scout shows an enemy kingdom's defences.
- Battle results move loot, items and "Owned" (permanently lost) units
  between kingdoms, so the defender must learn about them later
  (offline messages or kingdom data from the server).
- Search, Scout and Attack report back "a short time after the party has
  left": possibly server-side timers.
- Upgrade changes the kingdom's appearance (castle, transporter, mark,
  colour) "from those of other players when you're in N-Gage Arena
  mode", so the server shows other players' kingdoms on the map.
- Ranking: the All-Time Owner Board.

## Next steps

Rifts (retail card, PPP route):

1. Run Rifts' Arena mode with the server at log level 3. The log shows the
   SOAP login, the SegaChat login and every SNAP message.
2. If ArenaFramework stops after the room entry, compare the last
   messages with "Flow of Rifts" above (candidates: a room status
   notification, an answer for op 8, the encrypted login challenge).
3. Then the actions: server config, create/find/accept challenge, turns.

Pocket Kingdom:

1. Build the four DLLs with the N-Gage toolchain and fix what the
   compiler finds.
2. Run Pocket Kingdom with them and collect `PlArena.log`, together with
   the server log at level 3 (shows every GCA message).
3. Identify the remaining slots from the logged calls and the arenabase
   wrappers. Implement the SnapComm observer events (login result first,
   then lobby/room lists and entries), forward the HTTP slots as HTTP_REQ.
4. Pocket Kingdom game module on the server: kingdom storage, auctions,
   the Owner Board.
