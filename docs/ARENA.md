# N-Gage Arena support (work in progress)

Some N-Gage games, such as Pocket Kingdom: Own the World (UID 0x101fd3dd), do
not use GameComms at all. They ship the N-Gage Arena client stack (Nokia's
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
gamecommsd + gcs_native.c: Community SOAP and pages (TCP 80), SegaChat (TCP 5222), SNAP (UDP, logged)
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
   so the old values are saved; **Arena: restore modem** puts them back.
3. Bridge: nothing to set up; PPP mode is on by default (`ppp=1` in
   gcbridge.ini, `ppp_local` / `ppp_remote` change the addresses).
4. Server: `gamecommsd`; profile data goes to `arena_accounts`
   (default `gamecomms-arena.txt` in its working directory).
5. Start the game, choose N-Gage Arena and pick the PhantomLink access
   point when asked.

What the server answers (shapes taken from the parsers in the Arena DLLs,
see the reverse engineering notes below):

- Community SOAP: `checkImei`, `authenticateUser2` / `createUser` (true plus
  the user name as the first `xsd:string`, which the client uses for Jabber
  and in URLs; a session cookie `PL` + hex of the name), the `view*` and
  `edit*` account, display, alert, favourites and quote operations with the
  data kept per user, `changePassword`, the security question, and
  `errorCode 0` for every other operation.
- Device pages: `*_x.jsp` lists are empty lists, other `.jsp` pages and
  `/servlets/*` a stub page.
- SegaChat: stream, `jabber:iq:auth` get/set, empty roster, an empty result
  for every other iq; presence and messages are not routed yet.
- SNAP (UDP): logged as hex; the protocol is still unknown.

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
`im.prod.pk.nokia.sf.yav4.com`, `snap.prod.pk.nokia.sf.yav4.com` and
`ngage-auth` to 10.64.0.1 and keeps all other lines (its own lines carry the
tag `# PhantomLink Arena`). Because it is unverified whether the N-Gage
resolver uses the file for these names, the bridge also offers its own
DNS server through IPCP and answers it. Either path is enough on its own.

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
| Retail: PPP + IPv4 in every bridge | `protocol/src/gc_ppp.c`, `gc_ip.c`, `gc_net.c`, `firmware/src/bridge_core.c` | done, tested (PC bridge with Linux as the N-Gage) |
| Retail: native back ends | `server/src/gcs_native.c` | SOAP/pages/Jabber login answered, SNAP logged |

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

1. Build the four DLLs with the N-Gage toolchain and fix what the
   compiler finds.
2. Run Pocket Kingdom with them and collect `PlArena.log`, together with
   the server log at level 3 (shows every GCA message).
3. Identify the remaining slots from the logged calls and the arenabase
   wrappers. Implement the SnapComm observer events (login result first,
   then lobby/room lists and entries), forward the HTTP slots as HTTP_REQ.
4. Pocket Kingdom game module on the server: kingdom storage, auctions,
   the Owner Board.
