# N-Gage Arena support (work in progress)

Some N-Gage games, such as Pocket Kingdom: Own the World (UID 0x101fd3dd),
Rifts: Promise of Power (game class 8053), Ashen (game class 42318,
online leaderboards) and Worms World Party (game class 1104), do not use
GameComms for their online modes. They ship the N-Gage Arena client stack (Nokia's
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

## Overview

PhantomLink connects Arena games to the PhantomLink server without changing
the game card, over Bluetooth through a bridge or, where GPRS networks still
run, directly:

1. In the PhantomLink app, **Arena over BT** turns an access point named
   `PhantomLink` into a direct PPP connection over Bluetooth to the chosen
   bridge and writes the hosts file. **Arena over GPRS** only writes the
   hosts file, pointing the Arena names at the server set under Settings >
   Arena server (the server runs with `direct = 1`); the game then uses the
   operator's packet data access point. **Arena off** undoes either.
2. Every bridge (ESP32, PC, Android) carries its own PPP and TCP/IP stack.
   It answers the Arena host names with its own address and passes the
   game's connections and datagrams to the server.
3. The server answers in the Arena's own protocols: the Community web service
   (SOAP), SegaChat (Jabber, including its score service for leaderboards),
   SNAP (lobbies, game rooms) and the game servers of the individual games.

One Arena account works for all games. The first login with a new user name
creates the account and binds the user name, the password and the N-Gage
(its IMEI) together; later logins need the same password on the same N-Gage,
and one N-Gage can have one account. The server keeps the accounts in
`gamecomms-arena.txt` (passwords only as SHA-1). Details below.

### Supported games

| Game                          | Arena features                                                                     | Server side                                                        |                                Status                                |
| ----------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------ | :------------------------------------------------------------------: |
| Pocket Kingdom: Own the World | Battles against other players' kingdoms, auctions, rankings                        | Login, account and profile pages, SegaChat login, SNAP room, game server (kingdoms, battles, chat, Global Auction) | ![In%20progress](https://img.shields.io/badge/In%20progress-yellow) |
| Rifts: Promise of Power       | Challenges and turn-based battles "by e-mail" (melee, survivor, defender, KotH, CTF) | Login, SNAP lobby and room, Rifts game server (battles, turns)     | ![Untested](https://img.shields.io/badge/Untested-grey)             |
| Ashen                         | Online leaderboards: best score of every level and of the whole game               | SegaChat login, score reports and top lists (gcs_scores.c)         | ![Untested](https://img.shields.io/badge/Untested-grey)             |
| Worms World Party             | Play Now: Freestyle and Ranked game rooms, games between 2–4 players, skill points and rankings | Lobbies, room attributes and room search (gcs_snap.c), game data between the players, match ids, results, skill points and lists (gcs_scores.c) | ![Untested](https://img.shields.io/badge/Untested-grey) |

Pocket Kingdom logs in and reaches its Arena menus on a real N-Gage; its game
server (below) is tested against simulated games only. The Rifts
game server is tested against simulated games, but has not run with a real N-Gage
yet. Rifts' Bluetooth multiplayer works with the GameComms replacement
(see COMPATIBILITY.md), independently of Arena. Ashen's leaderboards are
tested against its decoder rebuilt from the game, not yet on a real
N-Gage; its Bluetooth multiplayer works as well.

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
  hardware; the plain password is accepted too); they are checked against
  the same account. A wrong password gets `<error code='401'>`. Games
  that log in to SegaChat only, without a Community login (Ashen), create
  the account with their first login, like the Habbo Console: without an
  IMEI, so the account is not bound to an N-Gage until a Community login
  of another game binds it.
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
Kingdom), `im01.rifts.bb.sf.yav4.com`, `bs01.rifts.bb.sf.yav4.com` (Rifts),
`im01.ashen.torus.sf.yav4.com` (Ashen), `im01.worms.paragon5.sf.yav4.com`,
`snap01.worms.paragon5.sf.yav4.com` (Worms World Party), `ngage-auth` and, not Arena, `builder.sulake.com` (Habbo Islands'
Habbo Console, docs/HABBO.md) to 10.64.0.1 (Bluetooth) or to the server's public address
(GPRS) and keeps all other lines (its own lines carry the tag
`# PhantomLink Arena`). Because it is unverified whether the N-Gage
resolver uses the file for these names, the bridge also offers its own
DNS server through IPCP and answers it for the Arena names. Either path
is enough on its own over Bluetooth; over GPRS, and for
`builder.sulake.com`, the hosts file is the only one (see below).

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
   listens, for a port forward such as 80 → 8000 (ports below 1024 need
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
| Retail: SegaChat score service (Ashen) | `server/src/gcs_scores.c` | written from sc_lib.dll and the game, tested against the game's decoder, **not yet on hardware** |

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
| 8 | 0xa000 | attribute (four letters), value (4 bytes BE, text for NAME) | set an attribute of the user's room: the server keeps it (up to 16 per room) and tells the other members (op 8 below); STAT is the room status of the room lists, MAXI the room size |
| 9 | 0xa000 | object id, attribute | read an attribute: op 9 {id, attribute, value}, then success |
| 72 | 0xb000 | {value, count, 3 bytes}, count × {attribute, operator, value} | room search with criteria (ArenaFramework's SelectGameRooms, "Find Game"): open rooms of the user's lobby that are not full and match; the operator's upper 3 bits give the value form (1: value and mask, 2: 16 byte text, else one word); seen: 0x21 STAT masked equal, 6 USER fewer users than the value (0x7fff: any), 1 equal (ARNA..ARND, LOID) |
| 10, 12, 13, 67 | | | user lists, user properties (not identified in detail) |
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
| 8 | room id, attribute, value | an attribute of the room changed (snapcomm.dll parser 0x39a0) |
| 72 | like op 11 | result of the room search with criteria (parser 0x3bf0) |
| 10 | header + count × {name [16], user id, property size, properties padded to 4 bytes} | user list |
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
| 10 | download challenge (unused placeholder) | battle, device | not implemented by the original server; rejected by this emulator |
| 11 | download characters | battle, user | count, the other players' characters |
| 12 | end game | battle, user, wins, losses, points | - |
| 13 | game upkeep | update flag; battle row, or battle, next device, my device + battle, turn | (no update) battle, server time, game over, dropped devices [8] + names, then the download turns answer |
| 14 | update friends | user, count, names | - |

Battle states: 0 waiting for players, 1 challenge deleted, 2 in progress,
3 out of time, 4 ended. Save slots: when a user reports its games
(challenge status), games in slots it no longer reports are given up
(host: challenge deleted, other players: withdrawn, running game: marked
as dropped).

## Ashen: online leaderboards

Ashen (6R21.APP, UID 0x101fd3e9) ships an older, smaller Arena stack than
Pocket Kingdom and Rifts: `arenafoundation.dll` (no ArenaFramework, no
arenabase, no HTTP DLL), `sc_lib.dll`, `scxmlparser.dll`, `snapcomm.dll`
and `sustandard.dll`. Its only Arena feature is a set of online
leaderboards, and it uses neither SNAP nor the Community web service for
them: everything goes through SegaChat. The server side is
`server/src/gcs_scores.c` (generic: any game that reports scores this way
gets leaderboards keyed by its game class id).

### Flow

1. The game opens a data connection (arenafoundation, RGenericAgent; no
   access point name is passed, so the phone asks or uses its default,
   e.g. `PhantomLink`).
2. It resolves `im01.ashen.torus.sf.yav4.com` and sends one UDP datagram
   `WAKE UP NETWORK!!! WAKE UP!!` to its port 40000 (nothing answers it;
   the server logs it).
3. SegaChat on TCP 5222: stream to `ngage-auth`, `jabber:iq:auth` with the
   user name and password typed into the game. Game class id: `42318`.
4. After a game, a score report; on the leaderboard screen, one query per
   board. The game shows at most 7 rows: `rank. name score` (score with 7
   digits).

### Messages (sc_lib.dll)

sc_lib builds both with jabberd's `jutil_msgnew` (no type attribute) and
puts the request kind into the id:

```
<message to='reporter@ngage-auth' id='segachat_send_event' event_type='submit'
         game_class_id='42318' match_id='' snap_name='USER' client_time='SECONDS'>
  <item name='level_1'><value>N</value></item> ... level_8, game_total
</message>

<message to='retrieval@ngage-auth' id='segachat_retrieve_req' event_type='topn'
         game_class_id='42318' snap_name='USER'>
  <retrieve><request>ITEMLIST (XML escaped)</request></retrieve>
</message>
```

The report carries nine items: `level_1` .. `level_8` and `game_total`,
values as decimal text, 0 for levels not played. No answer is expected.
ITEMLIST is

```
<itemlist><item name="queryid" value="3dabc" /><item name="board" value="HIGHSCORES" />
<item name="stat" value="level_N or game_total" /><item name="offset" value="0" />
<item name="limit" value="7" /><item name="periodicity" value="alltime" />
<item name="ordering" value="natural" /><item name="format" value="csv" /></itemlist>
```

The answer is a message without a type attribute (sc_lib drops typed
ones) with id `segachat_retrieve_resp`, the request's `event_type` and
`game_class_id`, and `<retrieve><response>CSV</response></retrieve>`.

### Answer (CSV)

Lines end with `\n`, fields are separated by `|` (6R21 0x9bf20 and the
line splitter at 0x9c354):

| Line | Fields | Read by the game |
|---|---|---|
| 1 | 1 | status as a number; anything but 0 shows line 2 as an error |
| 2 | 1 | message |
| 3 | 7 | field 3: the number of rows that follow |
| 4.. | 4 | name (up to 31 characters), rank, score; field 4 unused |

Numbers are read from their digits up to the end or a `.`; other
characters are skipped. Each line must have exactly that number of
fields, or the game reports an error. The game copies at most 1023 bytes
of the answer and keeps at most 10 rows on its stack without checking,
so the server never sends more than 10 rows (`GCSS_MAX_ROWS`). The
server's header is `queryid|board|stat|rows|offset|total|periodicity`
and its rows `name|rank|score|yyyy-mm-dd`; equal scores share a rank.

### Server rules

- The account name of the SegaChat login is used, not the `snap_name`
  the message claims (a different one is logged).
- Every player keeps the best (highest) value per statistic; 0 and
  negative values are not kept. `natural` lists the highest first,
  `reverse` / `ascending` the lowest. Only all-time lists exist; other
  periodicities get the all-time list.
- Data file `scores_data` (default `gamecomms-scores.dat`), lines
  `S <class> <stat> <user> <value> <time>`.

Unverified on hardware: the whole flow. In particular whether the game
needs anything after the login before it reports (it ignores everything
but the retrieve answer), and whether the CSV header fields other than
the row count meant something to the original server.

Tests: `tests/server/test_scores.c` decodes every answer the way the game
does (same splitter, number reader, field counts and limits) and runs a
SegaChat session through gcs_direct with the messages as sc_lib writes
them.

## Worms World Party

Worms World Party (6R39, UID 0x101fd3f3) is a Mophun game: 6R39.APP
starts `mophun.dll`, which runs `worms.mpn`. The game's online mode uses
N-Gage Arena through ArenaFramework 2.01.066 (the version in `arena.amr`),
whose Play Now screens do the lobby and room part; the server side is the
generic SNAP and SegaChat code plus the registration in
`server/src/gcs_wwp.c`. Tests: `tests/server/test_wwp.c`.

### Findings

- `arena.amr`: `JabberFQDN` im01.worms.paragon5.sf.yav4.com, `GameServerFQDN`
  snap01.worms.paragon5.sf.yav4.com, `JabberServicesName` retrieval1104,
  `JabberReportingName` reporter1104. The SegaChat requests go to these
  nodes (`retrieval1104@ngage-auth`), not to `retrieval@`; the server
  accepts both.
- `game.rsc` (a Symbian resource file): resource 2 links the settings,
  resource 3 holds key 0x2007 = **1104** (GameClassId; Pocket Kingdom has
  1 there, matching its SegaChat presence), 0x2000 = 6, two links to the
  settings of the two matchmaking modes (0x2019 and 0x201a), 0x2022 = 1,
  0x2009 = 1. Resource 4 (Freestyle): 0x200e = 2 and 0x200d = 4 (minimum
  and maximum players), the room fields Game name, Password, Game Mode
  (Beginner .. Going Up!), Terrain (Random, 1..15), Worms per team (1..4),
  Round time and Turn time. Resource 5 (Ranked): 2 players, Game name,
  Ranked, Equal Skill. No lobby name: the lobby list comes from the
  server, which offers one lobby per mode (`Freestyle`, `Ranked`).
- `mophun.dll` gives the Mophun program 33 `vArena*` functions (table at
  0x24c28, entries {function, argument count, name, 0xffff0000}). They call
  the "ArenaGlue" object that ArenaFramework ordinal 154 creates: game data
  to the room, private game data, game text, echo data, user property,
  match id, player stats and skills, top lists, proximity lists, and the
  fields of the retrieval answers (`vArenaGetNumRows`, `GetFieldValue`,
  `GetFieldIntValue`, which arenabase serves from the parsed CSV).
- `worms.mpn` is compressed (VMGP header flag 0x80 in byte 15: every
  section is `u32 compressed size` + an "LZ" block, an LZSS bit stream;
  `mophun.dll` 0x11838 and 0x11ac0). The data section unpacks; the code
  section is also encrypted (key from the game's licence, `mophun.dll`
  0x1bfec) and was not read. The data section names the report items:
  `win_lose`, `draw`, `opponent_id`, `gametime`, `roundsplayed`,
  `$matchmakingMode`, `checksum1`, `checksum2`, `medal`, `medal_val`, and
  the modes `freestyle` and `ranked`. The game shows "Points Total: %d (%d)"
  and "New Ranked Position: %d (%d)" after a ranked game.
- No game server: the turns travel as game data between the members of
  the room (SNAP ops 15 and 16).

### SegaChat services (arenabase.dll)

arenabase.dll builds the item lists (` <itemlist> <item name=".." value=".."/> ..`
with `$version` and a numeric `queryid`) and parses every answer the
same way (0x792c): the answer's `game_class_id` must be the game's class,
line 1 is the status (anything but 0: line 2 is the error), line 2 the
message, then lines of `|` separated fields (0x7774); all lines after
the first must have the same number of fields. The event type picks the
parser:

| Request | To | Items | Answer the parser accepts |
|---|---|---|---|
| `submit` (stats) | reporter | the game's items, `$version`, `queryid` | one line of 4 fields; field 3 goes to the game |
| `submit` (skills) | reporter | `$ps_skilltype` arena, `$ps_points` N, `$version`, `queryid` | the same |
| `getmatchid` | retrieval | `format`, `$version`, `queryid` | one line of 6 fields, field 1 the match id |
| `getplayer`, `topn`, `proximitylist` | retrieval | `board` (player_skills, highscores, hsfilter), `name`, `stat`, `offset`, `limit`, `below`, `periodicity`, `ordering`, `$matchmakingMode`, `format` csv | header + rows; header field 4 must be `playerskills`, `highscores` or `hsfilter`, header field 1 is read as a number |

| Board | getplayer | topn | proximitylist |
|---|---|---|---|
| playerskills | header 7, rows 6 | header 6, rows 7 | header 6, rows 7 |
| highscores | header 6, rows 4 | header 7, rows 4 | header 7, rows 4 |
| hsfilter | header 6, rows 3 | header 7, rows 3 | header 6, rows 3 |

Highscores top lists with a 7 field header and 4 field rows are the
shape Ashen's own parser reads, too. What the fields mean is decided by
the game's (encrypted) Mophun code; the server's choice follows Ashen's
rows:

- header `queryid|total|offset|rows|board|periodicity[|name or stat]`
- playerskills rows `name|rank|points|wins|losses|draws[|games]`
- highscores rows `name|rank|score|date`, hsfilter rows `name|rank|score`
- getmatchid `queryid|id|0|1|getmatchid|periodicity`, submit `queryid|1|submit|0`

Server rules (`gcs_scores.c`):

- `$ps_points` is added to the player's total (the game computes the
  points of a match; ArenaFramework passes them on unchanged), once per
  match id and player. A player starts at 0 points; getplayer for a
  player without ranked games answers rank 0.
- `win_lose` / `draw` count won, lost and drawn games, once per match id
  and player; `opponent_id`, `gametime`, `roundsplayed` and the checksums
  are only logged.
- Other numeric items are kept as best values (highscores, like Ashen).
- Match ids count up from a start value derived from the clock and are
  kept in the data file (`scores_data`), with the skill points (`K` lines)
  next to the best values (`S` lines).
- Ranks: most points first; equal points share a rank. proximitylist
  centres the list on the player.

### SNAP

Play Now uses the SNAP lobby list of the game class (ops 71 and 14),
lobby chat and users, creates rooms, sets the room's attributes (op 8:
the game settings and the room status), searches rooms with criteria
(op 72, "Find Game"), joins, and starts the game. The server keeps the
room attributes, tells the members about changes, and filters op 72 by
them (see the opcode tables above).

Not verified (no hardware run yet): the whole flow. In particular which
ops the Play Now screens send for ready states, chat and the game start
(the server answers the unidentified ops 5, 12, 13, 37, 67, 70, 73..75
with success and logs them), whether the room needs more than its
attributes, and the meaning of the playerskills fields. The server log
at level 3 shows every SNAP and SegaChat message for that.

## Pocket Kingdom: game server

Found in the retail game (6RAH.APP 110804, ARM code; static analysis of
the network code at 0x65ce0..0x68400 and the Global Auction screens at
0x2a000..0x2e000). The game creates its ArenaFramework with an observer
(AppUi+0x30) and keeps it at AppUi+0x1aa04. It uses these
CArenaFramework slots: 0x3c SendGameDataToServer(ptr, len), 0x48 own
user name, 0x5c/0x64 current room names, 0x6c join room by name ("Auction"
for the Global Auction, then back to the previous room), 0x70 opens the
Arena menus. Observer slot 0x38 is GameDataReceived (the parser at
0x67eb8), slot 0x08 the events 23, 27 and 28 (28: connection lost).

So Pocket Kingdom has a real game server behind the SNAP room
(op 17 in, op 15 out, gcs_snap.c). Battles are **not** played against
stored kingdoms, as the manual suggested: the defender's game is online
and answers scouting and attacks itself; the server passes the messages
on. Server code: `server/src/gcs_pk.c`, tests: `tests/server/test_pk.c`.

### Game class and rooms

game.rsc sets three flags (keys 0x2007, 0x2010, 0x201c) and "Scrimmage"
as lobby and room name (keys 0x200a, 0x200b); it has no class number.
The SegaChat presence carries `GameClassId='1'` on hardware, so the server
attaches the game server to class 1 (`GCS_PK_GAME_CLASS`) and lists the
lobby "Scrimmage". Not verified: the class the SNAP login carries (the
server also scans the login for it). Game data from a user who has not
entered a room yet reaches the game server, too.

### Messages

Byte 0 is the type, byte 1 a sequence number the answer echoes (not in
type 3). Names are 16 byte fields (15 characters, zero padded). Numbers
in auction messages are big endian.

| Type | Len | Direction | Content |
|---:|---:|---|---|
| 1 | 24 | game → server | online: [2] area, [3..6] kingdom look, name @8 → **129** (4 bytes, only counted) |
| 2 | 20 | game → server | keep alive (every ~165 frames), name @4 → **130** |
| 3 | 20 | game → server | going offline, name @4 (no answer) |
| 5 | 20 | game → server | moved: [2] new area, name @4 (no answer) |
| 6 | 20 | game → server | enemy search: [2] area, name @4 → **134** |
| 134 | 104 | server → game | up to 5 entries of 20 bytes from offset 4: name[16], look[4] (the castle, transporter, mark and colour other players see); empty name = end |
| 7 | 36 | relayed | scout: from @4, to @20 |
| 135 | 52 | relayed | scout answer from the defender's game: [2] result, to @4, from @20, party data |
| 9 | 88 | relayed | attack: [3] party, from @4, to @20, 4 units × 12 bytes from @40 |
| 137 | 88 | relayed | attack answer from the defender's game: [2] result, to @8, from @24, defending units |
| 10 | 36 | relayed | the battle starts: from @4, to @20 |
| 18 | 116 | relayed | chat: [2] area, from @4, to @20, text @36 (78 chars), [115] → **146** [2] = 1 delivered, 0 not |
| 13 | 32 | game → server | exhibit: name @4, item @20..26, starting price u32 @28 → **141** |
| 141 | 8 | server → game | [3] 0 exhibited (trigger041), 1 too many (040), 2 refused (043); u32 @4 auction id |
| 14 | 32 | game → server | browse: [2] category, [3] item type, [4] rarity, [8] sub-filter (0xff = any), u32 @12 page, name @16 → **142** |
| 142 | 248 | server → game | u16 @2 page, u32 @4 matches (0: trigger052), 10 entries from @8 |
| 15 | 28 | game → server | bid: u32 @4 auction, u32 @8 amount, name @12 → **143** |
| 143 | 8 | server → game | [2] 0 placed (044), 1 someone bid higher (045), 2 ended (046), 3 too many bids (042), 4 own exhibit (050), 5 refused (051); u32 @4 the bidder's previous bid on it: the game pays amount − this |
| 16 | 20 | game → server | own list: [3] mode 0 exhibits, 1 bids (looking), 2 exhibits, 3 bids (collecting), name @4 → **144** |
| 144 | 244 | server → game | 10 entries from @4 |
| 21 | 112 | game → server | kingdom summary: u32 @4 victory points, u32 @8 loot, unit table, parties, look, area → **149** |
| 17, 22 | 20, 32 | game → server | built by the game, never sent → 145, 150 |
| 255 | 4 | server → game | the game goes offline at once (never sent by this server) |

Auction list entry (24 bytes): u32 auction id, [4] status (0 = end of
list), [5..11] the exhibit's item bytes 20..26 (category, type, item,
three item values, level), u32 @12 seconds left (0: ended, the time is
not shown), u32 @16 price (highest bid or starting price), u32 @20 the
player's own bid.

| List | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| browse (142) | — | the player has the high bid | | |
| exhibits (144, modes 0/2) | running, no bids | running, bids | sold: the game adds @16 to the loot | no sale: the item comes back |
| bids (144, modes 1/3) | outbid | high bid | won: the game adds the item | lost: the game adds @20 back |

The game only books statuses 3 and 4 in the collecting modes (2 and 3);
it asks for mode 3 only when its inventory has room for the items it
bids on (trigger054), otherwise mode 1. The server forgets a result once
it has reported it in a collecting mode. The exhibit leaves the
inventory when it is sent and comes back if 141 refuses it.

Scout and attack results above 252 end the mission without a battle:
255 is what the defender's game sends when it cannot defend ("nobody
home"), 253/254 make the attacker's game report that the kingdom moved
area. The server answers with 254 when the defender is offline or in
another area.

### Server rules

- Online kingdoms: SNAP users of the game; the area and look come from
  messages 1, 5 and 6. The search answers up to five random online
  kingdoms of the same area. Kingdoms are kept by name (area, look, last
  summary, victory points, loot, last visit).
- Relayed messages go to the user named in the "to" field (any case);
  the server writes the sender's real name into the "from" field.
- Chat to a known kingdom that is offline is kept (at most 50 per player)
  and delivered after its next message 1; unknown names get 146 with 0.
- Auctions run 24 hours (`GCSP_AUCTION_SECS`); at most 10 exhibits and 10
  bids per player (results not yet collected count); prices up to
  9,999,999. A bid must be at least the starting price, or higher than the
  current price. The browse filter uses the category only (item type and
  rarity need the game's item tables), sorted by end time.
- Data file `pk_data` (default `gamecomms-pk.dat`), tab separated lines
  N (next auction id), K (kingdom), A (auction), B (bid), M (waiting chat).

Not implemented: bots ("Cannot chat with Bot!": the game seems to fill
the map with its own), the All-Time Owner Board (the Arena menus ask the
retrieval service for board `player_skills`; its answer format for this
game is unknown, the summary's victory points are stored for it).

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
  daily..alltime, through the Jabber retrieval/reporter services (the
  same services as Ashen's leaderboards, see above; gcs_scores.c answers
  them, but Pocket Kingdom's own items and boards are not known yet).
- SNAP: lobbies ("Scrimmage" is in game.rsc), rooms, matchmaking modes
  freestyle/ranked/filtered/challenge. The game enters a room named
  "Auction" by name.

From the manual:

- Arena mode is unlocked by the "Online Crest" from offline play.
  Online, players "challenge other players to battles and carry out
  auctions". The game box had a separate "N-Gage Arena Quick Start Guide
  for Pocket Kingdom" insert with more detail; worth finding.
- Battles run automatically from the party settings until one side is
  wiped out or the timer ends. (The guess that the server stores kingdoms
  for attacks was wrong: the defender's game answers online, see
  "Pocket Kingdom: game server".) Scout shows an enemy kingdom's defences.
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

Ashen (retail card, PPP or GPRS route):

1. Run the leaderboards with the server at log level 3: the log shows the
   SegaChat login, the score report items and every query and answer.
2. If the game reports an error, compare the answer with "Answer (CSV)"
   above.

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
4. Game server (done, gcs_pk.c): run the online map and the Global
   Auction with two N-Gages and the server at log level 3. Check the game
   class in the SNAP login, the area numbers, and whether the look bytes
   of the search answer come out in the right order.
5. The All-Time Owner Board (retrieval board `player_skills`).
