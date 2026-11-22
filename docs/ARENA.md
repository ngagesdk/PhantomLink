# N-Gage Arena support

This is work in progress.

Some N-Gage games, such as Pocket Kingdom: Own the World (UID 0x101fd3dd),
Rifts: Promise of Power (game class 8053), Ashen (game class 42318,
online leaderboards) and Worms World Party (game class 1104), do not use
GameComms for their online modes. They ship the N-Gage Arena client stack (Nokia's
version of SEGA's SNAP middleware) in their own `E:\System\Libs` and went
online over GPRS. PhantomLink replaces the three network clients of that
stack with plug-ins that use the PhantomLink Bluetooth bridge. The server
emulates the Arena back ends.

How the parts fit together:

- The game uses arenaframework.dll. That uses arenabase.dll and sc_lib.dll
  (SegaChat, a Jabber client). arenabase.dll uses SnapComm.dll and
  HttpDllRequest.dll.
- PhantomLink replaces SnapComm.dll, HttpDllRequest.dll and sc_lib.dll
  with its own plug-ins. They share one link through plarena.dll.
- plarena.dll talks over Bluetooth SPP to the bridge (ESP32, PC or
  Android, unchanged).
- The bridge talks over TCP to gamecommsd, where gcs_arena.c handles the
  Arena messages.

## Overview

PhantomLink connects Arena games to the PhantomLink server without changing
the game card, over Bluetooth through a bridge or, where GPRS networks still
run, directly:

1. In the PhantomLink app, Arena over BT turns an access point named
   `PhantomLink` into a direct PPP connection over Bluetooth to the chosen
   bridge and writes the hosts file. Arena over GPRS only writes the
   hosts file, pointing the Arena names at the server set under Settings >
   Arena server (the server runs with `direct = 1`); the game then uses the
   operator's packet data access point. Arena off undoes either.
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

| Game                            | Arena features                                                                                      | Server side                                                                                                                                     | Status   |
| ------------------------------- | --------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| Pocket Kingdom: Own the World   | Battles against other players' kingdoms, auctions, rankings                                         | Login, account and profile pages, SegaChat login, SNAP room, game server (kingdoms, battles, chat, Global Auction)                              | Working  |
| Rifts: Promise of Power         | Challenges and turn-based battles "by e-mail" (melee, survivor, defender, KotH, CTF)                | Login, SNAP lobby and room, Rifts game server (battles, turns)                                                                                  | Working  |
| Ashen                           | Online leaderboards: best score of every level and of the whole game                                | SegaChat login, score reports and top lists (gcs_scores.c)                                                                                      | Untested |
| Worms World Party               | Play Now: Freestyle and Ranked game rooms, games between 2 and 4 players, skill points and rankings | Lobbies, room attributes and room search (gcs_snap.c), game data between the players, match ids, results, skill points and lists (gcs_scores.c) | Untested |
| Pathway to Glory                | GPRS battles for up to 6 players, Field Radio voice messages                                        | SNAP lobbies and rooms, game server that passes the commands, synchronisation data and Field Radio messages on (gcs_ptg.c)                      | Working  |
| Pathway to Glory: Ikusa Islands | GPRS battles for up to 6 players, Field Radio voice messages                                        | Same as Pathway to Glory (gcs_ptg.c)                                                                                                            | Working  |

The status column matches the README.

Pocket Kingdom and Rifts, including their game servers, are tested on a
real N-Gage. Rifts' Bluetooth multiplayer works with the GameComms replacement
(see COMPATIBILITY.md), independently of Arena. Ashen's leaderboards are
tested against its decoder rebuilt from the game, not yet on a real
N-Gage; its Bluetooth multiplayer works as well.

## Retail games: PPP over Bluetooth (no DLLs replaced)

On a retail cartridge everything the game ships is read only, and its own
`E:\System\Libs` is searched before `C:`. `arenabase.dll` even loads
SnapComm.dll and sc_lib.dll from its own folder. The plug-ins below
therefore only work on a writable copy of the card. For retail cards the
original Arena stack stays in place and only its network path changes:

The path from the game to the server:

1. The game and the original Arena DLLs on the card use TCP/IP through
   the access point "PhantomLink".
2. The phone's CSD agent dials nothing (empty number) and runs PPP over
   BTCOMM::0. These phone settings are on C:.
3. The Bluetooth serial port carries the PPP data to the bridge (PC,
   ESP32 or Android). A first byte 0x7E puts the bridge into PPP mode
   (bridge_core.c).
4. In the bridge, gc_ppp handles LCP and IPCP (N-Gage 10.64.0.2, bridge
   10.64.0.1). gc_ip answers DNS (the Arena names resolve to 10.64.0.1)
   and terminates TCP and UDP.
5. The bridge sends GCN tunnel messages (gc_net.h) over its normal link
   to the server.
6. gamecommsd with gcs_native.c answers: Community SOAP and pages
   (TCP 80), SegaChat (TCP 5222), SNAP (UDP 9090, gcs_snap.c) and the
   game servers (Rifts: gcs_rifts.c).

No pppd, no root and no Python script are needed any more: every bridge
carries its own PPP peer and IPv4 end point (`protocol/src/gc_ppp.c`,
`protocol/src/gc_ip.c`), and the server runs the Arena services. The
bridge registers with the server for the N-Gage (API family 3, the
N-Gage's Bluetooth address as device id) and tunnels each TCP connection
and UDP datagram:

| GCN message      | Direction        | Content                                                  |
| ---------------- | ---------------- | -------------------------------------------------------- |
| `TCP_OPEN` 0xA0  | bridge to server | id, address and port the game connected to, its own port |
| `TCP_DATA` 0xA1  | both             | id, bytes (server to bridge: at most 512 per message)    |
| `TCP_CLOSE` 0xA2 | both             | id, FIN or reset                                         |
| `UDP` 0xA3       | both             | server side address and port, the game's port, bytes     |

Connection ids carry a generation byte, so late messages for a closed
connection never reach its successor. The bridge acknowledges TCP data from
the N-Gage only when the server link can take it, and takes data from the
server only when the TCP send buffer has room: the N-Gage's TCP and the
link's own flow control do the rest. When the server link comes back
fresh, the bridge registers again and resets the open connections.

Steps:

1. On the N-Gage: Settings > Connection > Access points > new access
   point PhantomLink, data bearer GSM data, any phone number.
2. PhantomLink app: choose the bridge, then Arena over Bluetooth. It
   changes that access point to a direct connection (empty number, no login
   script, addresses from the server), switches the data modem to
   `BTCOMM::0` / `BTCOMM`, points `BTCOMM::0` at the bridge
   (`CBTRegistry::SetDefaultCommPort`) and writes the hosts file. On
   Symbian 6.1 the modem record is shared by all data call access points,
   so the old values are saved; Arena off puts them back and removes
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
  file the pages of the Play menu (rankings, tournaments, activities,
  games) are generated (see "Rankings, tournaments and activities"
  below), other `*_x.jsp` lists are empty lists, other `.jsp` pages and
  `/servlets/*` use a themed fallback. Templates for all documented page
  routes and generated community lists are in `server/deploy/page-templates`;
  they are inactive until copied to `www_root`. The profile view is generated
  from the requested account instead of a file: it shows the username, names enabled
  in display options, favourites and quote, but not email or credentials.
  An invalid or unknown username returns 404. The addresses are
  ArenaFramework's, the same in every game; example pages and the list of
  addresses in `server/deploy/www`.
- SegaChat: stream, `jabber:iq:auth` get/set (the password is checked, see
  below), friends (roster, friend requests, presence), messages between
  users (see "Friends and chat" below), the score service of games with
  leaderboards, an empty result for every other iq.
- SNAP (UDP 9090): login, lobbies, game rooms, data between room members
  and the game servers behind the rooms, see "SNAP" below. Datagrams
  without the SNAP trailer are logged as hex.

### Accounts

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

The path from the game to the server:

1. The game and the original Arena DLLs on the card use TCP/IP through
   the operator's packet data access point.
2. `C:\System\Data\hosts` points the Arena names to the server's public
   IPv4 address.
3. The traffic goes through the operator network and the Internet.
4. `gamecommsd --direct=1` listens on TCP 80 and 5222 and UDP 9090
   (gcs_direct.c). It turns the traffic into the same GCN messages a
   bridge would send, one peer per client address.
5. gcs_native.c answers: Community SOAP and pages, SegaChat, SNAP and the
   game servers.

GPRS and Bluetooth players use the same accounts, lobbies and game
servers.

Steps:

1. Server: run `gamecommsd` with `direct = 1` on a machine with a public
   IPv4 address (or a router that forwards TCP 80, TCP 5222 and UDP 9090
   to it). The game always uses these three ports; `direct_http_port`,
   `direct_xmpp_port` and `direct_snap_port` only change where the server
   listens, for a port forward such as 80 to 8000 (ports below 1024 need
   privileges, see `server/deploy/gamecommsd.service`). `direct_bind`
   (default `0.0.0.0`) limits it to one interface. These listeners speak
   only the Arena protocols in plain text, like the original Arena; the
   bridge port keeps its own rules (loopback, access key).
2. On the N-Gage: a packet data access point for the operator (Settings >
   Connection > Access points, data bearer Packet data, the operator's
   APN). Most phones have one from the operator's settings message.
3. PhantomLink app: Settings > Arena server: the server's IPv4 address,
   or a host name (looked up when the next step runs, which opens a data
   connection; run it again if the address changes).
4. PhantomLink app: Arena over GPRS. It lists the packet data access
   points, writes the hosts file and asks for a restart.
5. Restart the N-Gage, then choose Arena over GPRS once more. With the
   entries already in place it looks up `arena.n-gage.com` and reports
   "Arena names: OK" when the answer is the server. Important: do not
   log in before it does. Without the hosts file the game would send the
   account name and password to whoever holds the old Arena domains today.
6. Start the game, choose N-Gage Arena and pick the operator's access point
   when asked. Arena off removes the hosts entries again (and restores
   the modem if Arena over BT changed it).

How the server tells clients apart: each client address is one peer of
`gcs_native`. Several N-Gages behind the same carrier NAT share an address,
so the server keeps every Community login per peer (not only the last one);
the session cookie picks the user for SOAP and page requests, and a SNAP
login is accepted for any account that logged in to the Community service
from the same address. Datagrams are only taken from addresses with a TCP
connection open or one in the last 10 minutes (`GCSD_IDLE_MS`), so spoofed
UDP sources are ignored. Silent TCP connections are closed after
`direct_idle_ms` (default 30 minutes; the SegaChat connection carries
nothing while a player sits in an offline screen, see "SNAP sessions of
idle players").

Not verified on hardware: whether the N-Gage's resolver uses the hosts file
for these names (the check in step 5 tells), and how carriers' NATs treat
the SNAP datagrams over longer games (the client sends regularly, which
keeps a NAT mapping open). Tests: `tests/server/test_direct.c` (in
process: logins, two N-Gages behind one address, SNAP, unknown sources,
idle addresses) and `tests/net/run_direct_test.py` (gamecommsd with real
sockets).

## Friends and chat (SegaChat, every Arena game)

ArenaFramework's Communicate menu (Friends, Add friend, Authorise / Do
not authorise, Remove, Chat) and the friends of Play Now (presence with
lobby and room, "join friend's game", challenges) use sc_lib.dll, a plain
libjabber client. The server (`gcs_native.c`, store `gcs_roster.c`, file
`roster_data`, default `gamecomms-roster.txt`) answers as a Jabber server
for the domain `ngage-auth`:

| Client sends                                                                                                | Server                                                                                                                                                                                                                                       |
| ----------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `<iq type='get' to='x@ngage-auth'>` `jabber:iq:browse` (id `segachat_browse_jid`, first step of Add friend) | `<user xmlns='jabber:iq:browse' jid='x@ngage-auth' name='x'/>`; sc_lib (0x356c) only goes on when the `user` child has a `name`, otherwise the menu stays in "Please wait"; unknown x: error 404                                             |
| `<iq type='get'>` `jabber:iq:roster` (id `segachat_get_roster`)                                             | the friends (`subscription='both'`) and the user's open requests (`'none'`, `ask='subscribe'`)                                                                                                                                               |
| `<presence to='x@ngage-auth' type='subscribe' id='segachat_buddy_req'>` (Add friend)                        | keeps the request, roster push to the sender, passes it on now or when x sends his first presence; unknown user: `type='unsubscribed'` back                                                                                                  |
| `type='subscribed'` (Authorise, id `segachat_buddy_accrej`)                                                 | both are friends: roster pushes `both` to both, `subscribed` to the asker, the current presences both ways                                                                                                                                   |
| `type='unsubscribed'` (Do not authorise)                                                                    | request dropped, roster push `remove` and `unsubscribed` to the asker                                                                                                                                                                        |
| `type='unsubscribe'`, or roster set `<item jid='x' subscription='remove'/>` (Remove friend)                 | the friendship ends on both sides (pushes, `unavailable` both ways)                                                                                                                                                                          |
| `<presence>` (with `<ext_pres .../>`, `<status>`)                                                           | kept and sent to every online friend; the first one also brings the friends' presences, open requests and waiting messages                                                                                                                   |
| `<presence type='unavailable'>`, end of stream, lost connection                                             | `unavailable` to the friends                                                                                                                                                                                                                 |
| `<message to='x@ngage-auth'>`                                                                               | to x with `from` set to the sender (a forged `from` is replaced); x offline: kept with a `jabber:x:delay` stamp if it has a `<body>` (at most 100 per user), messages without text (game data, challenges) are dropped; unknown x: error 404 |

Friendship is always mutual, as the Arena menus only know friends and
open requests. Messages to reporter@ / retrieval@ go to the score service
as before.

## Rankings, tournaments and activities (Play menu)

The Play menu's lists and pages (aamplayui.rsc) are device pages. Lists
are XML that cafmplay.dll parses (0x36c0):

```
<list><currentStart value="0"/><listTotal value="2"/>
  <itemNode index="0" contentId="1" gameId="42318" username="" mediaPath="">Ashen</itemNode>
</list>
```

`index`, `contentId` and `gameId` are numbers, `username` and `mediaPath`
text, the element text is the label. A chosen item opens the next page
with `contentId=`, `gameId=` or `username=` added; lists get `start=` and
`disp=` (paging), searches `cmd=search&username=`. `gcs_pages.c`
generates (paths below `/n-gage/device/`; a file in `www_root` with the
same path wins):

| Page                                                                                              | Content                                                                                                                               |
| ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `community/ranking_list_x.jsp`                                                                    | games with rankings (gameId = game class); with `cmd=search`: players whose name starts with `username`                               |
| `global/play/rankings/post_game.jsp?gameId=`                                                      | every ranking of the game: skill points (with won-lost-drawn) and the statistics of the score reports, top 10 and the user's own rank |
| `global/play/rankings/post_player.jsp?username=`                                                  | the player's ranks in every ranking ("View my rank": the user)                                                                        |
| `global/play/rankings/spotlight.jsp`                                                              | No. 1 of every ranking, latest activities                                                                                             |
| `community/tournament_list_x.jsp`                                                                 | tournaments: running, upcoming, finished (contentId = id)                                                                             |
| `global/play/tournaments/post_tournaments.jsp?contentId=`                                         | game, statistic, time, text, standings (top 10, own rank)                                                                             |
| `global/play/tournaments/news.jsp`                                                                | running (with the leader) and upcoming tournaments                                                                                    |
| `global/play/tournaments/winners.jsp`, `activities/winners.jsp`                                   | finished tournaments with the first three                                                                                             |
| `community/activity_list_x.jsp`, `activities/news.jsp`, `activities/post_activity.jsp?contentId=` | the latest 64 activities: new personal bests and won ranked games                                                                     |
| `community/game_list_x.jsp`, `global/play/games/post_game.jsp?gameId=`                            | the Arena games the server knows, with their leaders                                                                                  |

The rankings come from the SegaChat score service (`gcs_scores.c`):
Ashen's levels and total, the skill points and results of Worms World
Party and any other game that reports scores. Pocket Kingdom's All-Time
Owner Board shows up once its reports are known.

### Tournaments

The operator lists them in the file `tournaments` (default
`gamecomms-tournaments.txt`, example in `server/deploy/`), read again
whenever it changes:

```
# id|game class|statistic|mode|start|end|name|description
1|42318|game_total|best|2026-12-01|2027-01-01|Ashen December Cup|Best total score of December.
2|1104|$ps_points|sum|2026-12-01 18:00|2026-12-08 18:00|Worms week|Most skill points in ranked games.
3|1104|$wins|sum|2026-12-01|2026-12-31|Worms winners|Most ranked games won.
```

Mode `best` counts the highest single value, `low` the lowest (times),
`sum` adds up per match values (`$ps_points`, `$wins`). Times are UTC, the
end is excluded. Every player who reports a matching score during the
window takes part; there is no sign-up. Standings and activities are kept
in `tournament_data` (default `gamecomms-tournaments.dat`).

Not generated (still `www_root` or placeholders): downloads (wallpapers,
movies, screenshots, skins, sounds), upcoming games, message boards,
news (`/servlets/msg`).

## Parts

| Part                                                            | Files                                                                        | State                                                                                                 |
| --------------------------------------------------------------- | ---------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| GCA messages (types 0x80..0x9F)                                 | `protocol/include/gc_arena.h`, `protocol/src/gc_arena.c`                     | done, tested                                                                                          |
| Server Arena module                                             | `server/src/gcs_arena.c`                                                     | done, tested (generic)                                                                                |
| Shared link core (portable)                                     | `dll/arena/core/pla_core.c`                                                  | done, tested against the server core                                                                  |
| plarena.dll (Bluetooth, log)                                    | `dll/arena/symbian/PlArena.cpp`                                              | written, not built yet                                                                                |
| SnapComm.dll                                                    | `dll/arena/symbian/PlSnapComm.cpp`                                           | logs all slots, forwards login                                                                        |
| sc_lib.dll                                                      | `dll/arena/symbian/PlScLib.cpp`                                              | logs all slots                                                                                        |
| HttpDllRequest.dll                                              | `dll/arena/symbian/PlHttpRequest.cpp`                                        | logs all slots                                                                                        |
| Retail: access point + hosts                                    | `app/src/ArenaSetup.cpp`                                                     | done, works on an N-Gage                                                                              |
| Retail over GPRS: hosts + check (app), direct clients (server)  | `app/src/ArenaSetup.cpp`, `server/src/gcs_direct.c`                          | written; server tested, app syntax-checked only, not yet on hardware                                  |
| Retail: PPP + IPv4 in every bridge                              | `protocol/src/gc_ppp.c`, `gc_ip.c`, `gc_net.c`, `firmware/src/bridge_core.c` | done, tested (PC bridge with Linux as the N-Gage)                                                     |
| Retail: native back ends                                        | `server/src/gcs_native.c`                                                    | SOAP/pages/Jabber login answered                                                                      |
| Retail: shared accounts                                         | `server/src/gcs_accounts.c`                                                  | done, tested                                                                                          |
| Retail: SNAP lobby/room service                                 | `server/src/gcs_snap.c`                                                      | written from the client DLLs, tested against simulated games and on an N-Gage (Pocket Kingdom, Rifts) |
| Retail: Rifts game server                                       | `server/src/gcs_rifts.c`, `server/src/gcs_rcodec.c`                          | written, tested against simulated games and on an N-Gage                                              |
| Retail: SegaChat score service (Ashen)                          | `server/src/gcs_scores.c`                                                    | written from sc_lib.dll and the game, tested against the game's decoder, not yet on hardware          |
| Retail: SegaChat friends, presence, chat                        | `server/src/gcs_roster.c`, `server/src/gcs_native.c`                         | written from sc_lib.dll's stanzas, tested against simulated clients, not yet on hardware              |
| Retail: rankings, tournaments, activities, games (device pages) | `server/src/gcs_pages.c`, `server/src/gcs_tourney.c`                         | list format from cafmplay.dll, tested with its decoding rules, not yet on hardware                    |

The DLLs build only with `-DGC_BUILD_ARENA=ON` (off by default until the
build has been verified with the N-Gage toolchain). Copy `plarena.dll`,
`snapcomm.dll`, `sc_lib.dll` and `httpdllrequest.dll` over the game's
copies in `E:\System\Libs`. Every call is logged to
`C:\System\Data\PlArena.log` with its arguments and the caller's return
address. That log is the next step for identifying the remaining slots.

## Not tied to one game

- The server keeps users, lobbies and rooms per game class id (sent
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

One UDP socket. The login goes to port 9090 of the server named
`GameServerFQDN` in `arena.amr`; the login answer can move the client to
another port (the server keeps 9090). The client only accepts datagrams
from the address and port it sends to. A datagram is one or more
messages followed by the trailer `BA 47 66 xx` (the client sends `11`
and checks only the first three bytes); it holds at most 896 bytes.

Message header, 16 bytes, big endian:

| Offset | Size | Content                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ------ | ---- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0      | 2    | flags and length: 0x8000 reliable, 0x4000 the ack field is valid, 0x2000 single packet (`KK_SINGLE_PACKET_DEF` in the game server's kkGSApi.h; without it byte 2 is the `PacketSeq` of a series, counting down to 0, and ArenaFramework takes a list as complete only at 0; both sides set it on every message), 0x1000 target is the lobby (else the game room), 0x0800 another message follows in the same datagram (the client numbers only the first reliable message; reliable messages packed behind it carry sequence number 0, share its acknowledgement and are processed with it), 0x0400 game settings data; bits 0..9 the length including the header                                                                                                                                                                                                                                                                                                          |
| 2      | 1    | command sequence of the request; answers echo it (ArenaFramework matches attribute changes by it)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 3      | 1    | opcode                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| 4      | 4    | user id (from the login answer)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| 8      | 4    | sequence number: reliable messages count from 0 after the login, unreliable ones are dropped when older than the last. snapcomm.dll numbers one reliable message per datagram; others carry 0 (packed behind it, or sent alone, like Rifts' room attributes ARNC and ARND): the server processes those once and tells repeats by opcode and command sequence. Messages queued at the same time go out as a series: the first numbered with 0x0800, the rest in datagrams of their own with whatever was in the buffer in sequence and ack (0, 1, 6, 0x03000000, ...; Pathway to Glory's 9 clock pings at the battle start, hardware 2026-11-22). Until the next numbered message the server takes a reliable message without 0x0800 whose sequence number is not the next or an early one as part of the series: once when the series was new, not at all when it was a repeat; game data (op 17) even when identical, other ops told apart by opcode and command sequence |
| 12     | 4    | acknowledged reliable sequence number                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |

Every reliable message is acknowledged on its own (the client sends a
16 byte message with flags 0x6010, opcode 0 and the sequence number in
the ack field) and resent until then (`MAX_RETRIES` 17, then the
connection counts as lost). Reliable messages are delivered in order.

### Opcodes

Requests (client to server):

| Op                         | Flags           | Payload                                                                                  | Meaning                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| -------------------------- | --------------- | ---------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 44                         | 0x3000          | user name [40], [60] text, game class (BE) at 100, ..., TLV `LC`/`MR` (latency, retries) | login                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| 2                          | 0xb000          | -                                                                                        | logout                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 3                          | 0xb000          | name [16]                                                                                | lobby by name                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| 14                         | 0xb000          | -                                                                                        | lobby list                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| 4                          | 0xb000          | name [16], max users, password [16], two words                                           | create game room                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| 5                          | 0xb000/0xa000   | id                                                                                       | (not identified)                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| 6                          | 0xb000          | lobby id (or name [16] + id)                                                             | enter lobby                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| 6                          | 0xa000          | room id, password [16], word                                                             | enter game room                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| 7                          | 0xb000/0xa000   | -                                                                                        | leave lobby / room                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| 8                          | 0xa000          | attribute (four letters), value (4 bytes BE, text for NAME)                              | set an attribute of the user's room: the server keeps it (up to 16 per room) and tells the other members (op 8 below); STAT is the room status of the room lists, MAXI the room size                                                                                                                                                                                                                                                                                                   |
| 9                          | 0xa000/0xb000   | object id, attribute                                                                     | read an attribute of a room or a lobby (0xb000; NAME as 16 byte text, USER, MAXI): op 9 {id, attribute, value}, then success. ArenaFramework's slot 0x6c reads the NAME of the current lobby this way                                                                                                                                                                                                                                                                                  |
| 73                         | 0xb000          | like op 72                                                                               | room search with criteria (snapcomm.dll 0x81f0, arenabase slot 0xf4; ArenaFramework's SelectGameRooms, Pocket Kingdom's Ranked "Finding game room" and slot 0x6c): open rooms of every lobby of the game class that are not full and match; 40 byte entries with the lobby (parser 0x3c24). Seen: 0x21 STAT masked equal, 6 USER fewer users than the value (0x7fff: any), 1 equal (ARNA..ARND, LOID)                                                                                  |
| 72                         | 0xb000          | {value, count, 3 bytes}, count x {attribute, operator, value}                            | lobby search with criteria (snapcomm.dll 0x7fb4, arenabase slot 0xf0; ArenaFramework's slot 0x6c sends NAME = "Auction"): lobbies of the game class that match, answered like op 71 (36 byte lobby entries, parser 0x3bf0, then 0x38a0). The operator's upper 3 bits give the value form (1: value and mask, 2: 16 byte text, else one word). Criteria on room attributes (STAT, ARNx) still get the old room search of the user's lobby (Worms World Party's "Find Game", unverified) |
| 10, 12, 13, 67             |                 |                                                                                          | user lists, user properties (not identified in detail)                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 11                         | 0xb000          | lobby id                                                                                 | search game rooms                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| 71                         | 0xb000          | -                                                                                        | lobbies of the login's game class (Rifts sends it when the player creates a challenge)                                                                                                                                                                                                                                                                                                                                                                                                 |
| 15                         | 0xa400 / 0x2000 | data                                                                                     | game data to the room, reliable / unreliable                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| 16                         | 0xa400 / 0x2000 | count, user ids, data                                                                    | game data to some users                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| 17                         | 0xa000          | data                                                                                     | game data to the room's game server (`SendGameDataToServer`, CArenaFramework vtable slot 0x3c)                                                                                                                                                                                                                                                                                                                                                                                         |
| 18, 19, 20, 50, 37, 70..75 |                 |                                                                                          | not used by the Rifts flow                                                                                                                                                                                                                                                                                                                                                                                                                                                             |

Answers and notifications (server to client):

| Op  | Payload                                                                                                | Meaning                                                                                                                             |
| --- | ------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| 41  | port (0: keep), -, user id                                                                             | login accepted                                                                                                                      |
| 39  | request op, reason                                                                                     | request failed (op 39 with request op 1 = login refused)                                                                            |
| 40  | request op, value, ...                                                                                 | request succeeded; the 0x1000 flag tells lobby from room answers (op 6: "Entered lobby" / "Entered room", value = id)               |
| 3   | name [16], lobby id                                                                                    | a lobby (ArenaFramework builds its lobby list from these)                                                                           |
| 4   | room entry                                                                                             | the created room                                                                                                                    |
| 11  | header {parent, total, count} + count x room entry (name [16], users, lobby, status, max users, id)    | room search result; ArenaFramework 2.01.047 (parser 0x3954) keeps users, status, max and id and calls a room full when users >= max |
| 14  | header + count x {name [16], id, members, max}                                                         | lobby list                                                                                                                          |
| 71  | header {game class, total, count} + count x {name [16], users, -, max, status, lobby id}               | lobbies of the game class; no op 40 follows (ArenaFramework would start a new, empty list with it)                                  |
| 8   | room id, attribute, value                                                                              | an attribute of the room changed (snapcomm.dll parser 0x39a0)                                                                       |
| 72  | like op 11                                                                                             | result of the room search with criteria (parser 0x3bf0)                                                                             |
| 73  | header {0, total, count} + count x 40 byte entries (name [16], lobby, users, -, status, max users, id) | result of the room search with criteria over all lobbies                                                                            |
| 10  | header + count x {name [16], user id, property size, properties padded to 4 bytes}                     | user list                                                                                                                           |
| 6   | {name [16], user id, property size}                                                                    | a user entered the room                                                                                                             |
| 7   | user id                                                                                                | a user left the room                                                                                                                |
| 15  | data                                                                                                   | game data (also what the game server sends; arrives as `GameDataReceived`)                                                          |

### Flow of Rifts (game.rsc)

Rifts sets `GameClassId` 8053, `BypassCommunity`, `LobbyToJoin` "S79",
`RoomToJoin` "BYPASS", `CreateRoomToJoin` 1 and
`ReturnContextOnRoomEntry` 1. The flow:

1. Community login (SOAP), then SNAP login, then SegaChat login (seen on
   hardware).
2. Creating a challenge sends op 71 (lobbies of the game class).
   ArenaFramework looks for "S79" in that list.
3. Op 13 and enter lobby (op 6, packed).
4. Room search (op 11).
5. Op 13 and create room "BYPASS<user id>" (op 4, packed).
6. Room attributes (op 8: ARNA, ARNB, ARNC, ARND). Creating the room
   counts as entering it: the server makes the creator a member, there
   is no join. ArenaFramework goes on only when all four are confirmed.
7. Room status (op 8 STAT 0x90000000).
8. `EGameStart`, then game data to the game server (op 17). Answers come
   as op 15.

The server creates lobbies on demand, gives every created room a game
server when one is registered for the login's game class, and closes
empty rooms.

This flow works on a real N-Gage. ArenaFramework needs nothing more after
the room entry before `EGameStart`, and the login answer is enough without
the encrypted challenge (op 64 answered by op 65, done with sustandard.dll)
that the client also understands. The server log at level 3 shows every
message in both directions.

### SNAP sessions of idle players

snapcomm.dll runs its protocol (acknowledgements, resends) only when the
game pumps ArenaFramework (slot 0x1c, which calls arenabase slot 0x64, then
snapcomm slot 0x90, then the poll at 0x32e4). Pocket Kingdom pumps from its
main tick (6RAH 0x6f3c4) every frame, also in the offline screens; only a
loading screen (AppUi+0x1b04c set, 0x73608) skips it. The game has no
keep alive of its own (see "No keep alive in Pocket Kingdom" below), so an
idle player sends nothing for minutes. The server
used to end such a session after 15 unacknowledged resends (about 15 s,
as soon as anything reliable was sent to the player, e.g. a room
notification) or after 3 minutes of silence, and then ignored the
player's datagrams, so the game lost the connection when it came back.

Now (`gcs_snap.c`): resends back off (1, 2, 4, then every 8 s) and never
end a logged-in session; the first datagram after a quiet spell makes the
server resend everything still unacknowledged at once; a session ends on
logout, on a lost bridge (`gcs_snap_peer_gone`) or after 30 minutes of
silence (`GCSS_IDLE_MS`). The log shows `user ... gone (silent)` for the
last case and `no acknowledgement for N s, still waiting` (level 2) when a
client stalls. Up to 32 reliable messages wait per session; more are
dropped with `send queue full`.

Still not solved: on hardware (2026-11-17) players are dropped after a
few minutes in the offline screens with the change above.

### No keep alive in Pocket Kingdom

Static analysis of 6RAH.APP 110804 and snapcomm.dll (2026-11-22): the
client keeps nothing alive and watches for nothing, at any layer.

- Game messages: type 2 (sender 0x65dd8: a sequence byte and 16 bytes of
  ArenaFramework slot 0x48) is sent only once in the online start-up
  (0x6b2a4 in 0x6b234, which waits up to 164 frames for the answer) and
  once before the Global Auction (0x2a5f4). No game message is sent on a
  timer.
- The parser (0x67eb8) only counts 129, 130 and 149 (AppUi+0x1ab6c,
  0x67da4) and ignores 132, 145 and 150. Nothing records when the server
  was last heard.
- snapcomm has no idle keep alive and no silence timeout. Its timers: the
  login (state +0x8c, 100 s, cleared by op 41 at 0x4c28, else op 39 with
  reason 20 from 0x3de8) and the resend of the client's own reliable
  messages (0x268c): the timeout starts at INITIAL_LATENCY (3000 ms),
  is re-estimated from the round trip on every acknowledgement
  (0x8a10), grows by 200 ms per try, and after MAX_RETRIES (6) tries
  the client sets error 300 and gives up.
- Every reliable datagram is acknowledged before dispatch (0x2eec calls
  0x88c8); with nothing else queued, snapcomm sends a 16-byte
  acknowledgement of its own. This happens in the poll, so every frame.
- A leave in the pump makes the game go offline (0x6f5d0 -> 0x723e0);
  observer event 28 (slot 0x08, 0x67bb8; raised by ArenaFramework at
  0x6092 and 0x96b4) tears ArenaFramework down (0x721e4).

So staying in a menu times nothing out on the phone. The client drops
only when one of its own reliable messages stays unacknowledged for 6
tries (roughly 20-40 s on BT/PPP), or on a socket or link error. A drop
after idling therefore means the path died during the quiet spell
(the phone's idle timeout for the data connection, the PPP link or the
bridge).

If the server should keep the path busy, it can send a reliable SNAP
message whose op snapcomm hands to its default case (0x517c, e.g. op 0):
the phone acknowledges it on its next poll and drops it. Not an
unsolicited 130: the start-up and auction state machines would take it
as an answer. Not implemented.

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

| Id  | Message                | Layout                                                                                                                  |
| --- | ---------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| 4   | MSG_SERVER             | object id i32 @4, action type u8 @8, flags u8 @9, offset i32 @12, length i32 @16, total i32 @20, data [at most 384] @24 |
| 5   | MSG_SERVER_CANCEL      | object id @4, reason @8                                                                                                 |
| 25  | MSG_FLOW_EXTEND_CREDIT | credit @4                                                                                                               |
| 26  | MSG_PING               | answered with MSG_PING                                                                                                  |

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

| Type | Action                                  | Request                                                                                                  | Answer                                                                                                                   |
| ---- | --------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| 0    | download turns                          | battle, turn                                                                                             | count, {length, message} after that turn                                                                                 |
| 1    | turn count                              | battle, turn                                                                                             | count (+ device of the last end-turn message)                                                                            |
| 2    | upload turns                            | battle, turn, my device, count, {length, message}                                                        | bool                                                                                                                     |
| 3    | upload characters                       | battle, team, count, characters, save slot, power                                                        | character ids                                                                                                            |
| 4    | create challenge                        | battle row + upload                                                                                      | battle id, character ids                                                                                                 |
| 5    | server config                           | power rank version, save version, protocol                                                               | {2, 0, version}, power rank version, versions (echoed), class and item power tables when the version differs             |
| 6    | name validation                         | string                                                                                                   | int (0 = offensive)                                                                                                      |
| 7    | find challenge                          | paging data, min/max power, kind (-1 any), players, friends only, ranked only, name [16], max rows, user | count, battles, their players, 4, player count                                                                           |
| 8    | accept challenge                        | upload                                                                                                   | result (0 waiting, 1 last player: the battle starts, 2 full, 3 error, 4 gone, 5 team taken), device, seed, character ids |
| 9    | challenge status                        | user, count, {battle, turn, save slot}                                                                   | count, {state, turns available, next device}                                                                             |
| 10   | download challenge (unused placeholder) | battle, device                                                                                           | not implemented by the original server; rejected by this emulator                                                        |
| 11   | download characters                     | battle, user                                                                                             | count, the other players' characters                                                                                     |
| 12   | end game                                | battle, user, wins, losses, points                                                                       | -                                                                                                                        |
| 13   | game upkeep                             | update flag; battle row, or battle, next device, my device + battle, turn                                | (no update) battle, server time, game over, dropped devices [8] + names, then the download turns answer                  |
| 14   | update friends                          | user, count, names                                                                                       | -                                                                                                                        |

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

| Line | Fields | Read by the game                                            |
| ---- | ------ | ----------------------------------------------------------- |
| 1    | 1      | status as a number; anything but 0 shows line 2 as an error |
| 2    | 1      | message                                                     |
| 3    | 7      | field 3: the number of rows that follow                     |
| 4..  | 4      | name (up to 31 characters), rank, score; field 4 unused     |

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
  resource 3 holds key 0x2007 = 1104 (GameClassId; Pocket Kingdom has
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

| Request                              | To        | Items                                                                                                                                                  | Answer the parser accepts                                                                                            |
| ------------------------------------ | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| `submit` (stats)                     | reporter  | the game's items, `$version`, `queryid`                                                                                                                | one line of 4 fields; field 3 goes to the game                                                                       |
| `submit` (skills)                    | reporter  | `$ps_skilltype` arena, `$ps_points` N, `$version`, `queryid`                                                                                           | the same                                                                                                             |
| `getmatchid`                         | retrieval | `format`, `$version`, `queryid`                                                                                                                        | one line of 6 fields, field 1 the match id                                                                           |
| `getplayer`, `topn`, `proximitylist` | retrieval | `board` (player_skills, highscores, hsfilter), `name`, `stat`, `offset`, `limit`, `below`, `periodicity`, `ordering`, `$matchmakingMode`, `format` csv | header + rows; header field 4 must be `playerskills`, `highscores` or `hsfilter`, header field 1 is read as a number |

| Board        | getplayer        | topn             | proximitylist    |
| ------------ | ---------------- | ---------------- | ---------------- |
| playerskills | header 7, rows 6 | header 6, rows 7 | header 6, rows 7 |
| highscores   | header 6, rows 4 | header 7, rows 4 | header 7, rows 4 |
| hsfilter     | header 6, rows 3 | header 7, rows 3 | header 6, rows 3 |

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

## Pathway to Glory: Ikusa Islands

Pathway to Glory: Ikusa Islands (6R82, UID 0x101fd41d, v1.05)
plays its GPRS games through N-Gage Arena: ArenaFramework 2.01.080 does
login and the Play Now screens, and a game server of RedLynx's own sits
behind the game room. The server side is `server/src/gcs_ptg.c` (game
class 2955) on top of the SNAP server; the first Pathway to Glory is
nearly the same (see below). Tests: `tests/server/test_ptg.c`.
Bluetooth play does not use GameComms (own RFCOMM, see
[COMPATIBILITY.md](COMPATIBILITY.md)); this section is only about GPRS.

Everything below comes from static analysis of the retail dump (6R82.APP
is ARM code; arenaframework.dll, arenabase.dll and snapcomm.dll
"Jun 22 2005" are Thumb). Nothing has run on a real N-Gage yet.

### Findings

- `arena.amr`: `JabberFQDN` im.ptg2.redlynx.sf.yav4.com, `GameServerFQDN`
  bs01.ptg2.redlynx.sf.yav4.com, `JabberServicesName` retrieval2955,
  `JabberReportingName` reporter2955. Ranked results go through the
  generic SegaChat reporter (`gcs_scores.c`).
- `game.rsc`: key 0x2007 = 2955 (game class), 0x200d = 6 (players).
  Two matchmaking modes (0x2018 and 0x201a). Settings: Battleground (City,
  Jungle, Island, City 1..3, Jungle 1..3, Island 1..3), Number of
  Missions 1..3, Ranked, Game name ("Battle"), Turn time 30..90 s, players
  2/4/6, soldiers 3..8. No lobby names, so the server offers
  `Freestyle,Ranked`. The game reads the settings back in observer slot
  0x40 (6R82.APP 0x858b4).
- The game imports only three ArenaFramework ordinals: 164
  (`CArenaFramework::NewL(env, observer)`, 0xc38c) and two descriptor
  helpers (59, 129). Everything else goes through the framework's vtable.
- No direct sockets for GPRS. The ESOCK imports belong to the game's
  own Bluetooth stack. All game data goes to the game server with
  CArenaFramework slot 0x3c (arenabase 0xe8, reliable, SNAP op 17) and
  comes back as room data. This snapcomm build delivers op 15 (dispatch
  0x42ac, op 15 at 0x4910; arenabase registers events 0x12..0x15), unlike
  Pocket Kingdom's, so the server answers with op 15.
- Host. ArenaFramework's IsHost (slot 0x58, 0x10cb0) compares its own
  user id with the room attribute `HOST`, which it asks with op 9 when it
  enters a room. gcs_snap.c now answers `HOST` with the room's creator
  (0 for fixed rooms, as before), unless a game set it with op 8.

### Network layer ("Pathway To Glory Network", 6R82.APP 0x83fd4)

One object (0x8f8 bytes, vtables 0x1b0908 / 0x1b0944 observer /
0x1b0990) with one receive queue per message type (55 types, 12 bytes
each at +0x48). Main vtable: +0x08 receive into a buffer, +0x0c receive
into a new buffer, +0x10 send, +0x18 flush all queues and drop the sync
streams, +0x1c state.

Message, both directions, little endian:

| Offset | Size | Meaning                                                               |
| ------ | ---- | --------------------------------------------------------------------- |
| 0      | u16  | bits 0..6 type, bit 7 "restart counters", bits 8..14 tag, bit 15 zlib |
| 2      | u16  | data length (compressed: length of the zlib stream)                   |
| 4      |      | data (compressed: u32 unpacked size, then the zlib stream)            |

- The game sends one message per framework call (send 0x844b4, at most
  0xffc data bytes), never split or compressed.
- From the server (receive 0x84b9c, observer slot 0x34): a message longer
  than 600 bytes comes in pieces of the same header; the first carries
  the total length, then 596 data bytes per piece (compressed: the first
  piece 592 after the u32 size). Compressed messages are inflated with
  the game's zlib 1.1.4 (at most 200000 bytes unpacked).
- Type 32 and type 34 are bundles of `{u16 length, data}` entries; the
  game puts them into the queues of type 16 and type 21. After every
  type 34 it queues an empty 35, after every 23 an empty 41. Quirk: a
  type 34 that is neither split nor compressed is queued as it is, so the
  server always sends 34 compressed.
- For every 23, 32 and 34 the game answers type 33 {u16 count of 23/34,
  u16 count of 32, u16 tag of the message, u16}. Bit 7 resets both counts
  and the drop flag that slot +0x18 sets; the server sets it on every
  message so nothing is dropped after the game flushed its queues.
- **The game server speaks first.** After ArenaFramework's game start
  (event 25) the network layer stays idle until the first message from
  the game server arrives (start of the receive function, 6R82.APP 0x84bac,
  6R72.APP 0x79888: flag +0x8f6, observer slot 0x48 with 3); only then
  does the game send anything. Without it the phone hangs on "Starting
  Game" (seen on hardware 2026-11-17: the whole room set-up, STAT
  0x90000000, op 13, op 12, then silence). The server pings (type 18)
  every phone of a room whose status has bit 31 (set by the host when it
  starts the game, CArenaFramework 0x7a0c) every 2 s until the phone sends
  game data other than the empty answer.
- Type 18 from the server: the game answers 19 without data. Every 20 s
  the game sends 19 with 4 bytes and expects 19 back within 30 s, else
  "Connection to game server has been lost" (0x84a80).
- Type 50 {u16 code}: the game ends or shows an error. Codes (texts at
  0x1a5054, switch 0x6927c): 1 server asserted, 2 game ended, 3 host left,
  4 GPRS too bad, 5 too many players, 6 client build not supported,
  7 connection lost, 8 no opponents, 9 removed by the host, 10 connecting
  failed, 11 no version from the client, 12 timeout. 0, 3, 5, 7, 8 and 9
  end the game.

### What travels (Bluetooth host code as the reference)

In a Bluetooth game the host's phone is the hub (multiplayer loop
0x2e7a0): clients send everything to it, and it passes on what the others
need. Over GPRS each phone has one connection, to the server, so the
server does the passing on:

| Type                         | Meaning                                                                                      | Host's Bluetooth code                                 | Server                                                                                      |
| ---------------------------- | -------------------------------------------------------------------------------------------- | ----------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| 16                           | a soldier's command (serialised, `IssueCommand` 0x2e200)                                     | executes it and sends it to all other connections     | to all other players, in bundles (32)                                                       |
| 21, 35                       | synchronisation records, end marker (0x271d8, waits 0x74468)                                 | sends records, then 35                                | collects the host's records, sends one 34 at the 35; without 35 after 3 s the records alone |
| 23, 41                       | permanent damage data ("Waiting permanent damage data from the host"), end marker (0xe0da8)  | sends 23, then 41 to all                              | 23 to the clients, swallows the host's 41 after a 23                                        |
| 43                           | Field Radio chunk, 260 bytes: player (1..20), chunk, chunk count, 256 bytes of the recording | passes it to the players of the side whose turn it is | to all other players; a repeated copy within 1.5 s once                                     |
| 52                           | message for everybody                                                                        | passes it on to all others                            | to all other players                                                                        |
| 27, 39, 54                   | player ended / host ended, "Overpowered bonus!"                                              | handles them itself                                   | to all other players (no phone is the host in a GPRS game)                                  |
| 15                           | barrier {u32 1, u32 start time}                                                              | collects one from every client, answers all           | the server collects and answers, see "GPRS game"                                            |
| 31                           | clock ping, 20 bytes                                                                         | fills in its clock, sends it back                     | the server answers                                                                          |
| 0, 9, 10, 11, 12, 13, 14, 37 | GPRS lobby, see below                                                                        | (Bluetooth: the host's lobby, 0x8eb3c)                | the server runs the lobby                                                                   |
| others                       |                                                                                              |                                                       | from a client to the host, from the host to the clients                                     |

The Field Radio ("Use the Field Radio for voice communication between
you and your allies", in-game menu > Communications) records 8 kHz audio
(MEDIACLIENTAUDIO, WAV writer 0x103800) and streams the message in
256-byte chunks, one every 30 frames (sender 0x2f078); the receiver
collects the chunks and plays the message when all are in (0x2ec60).

Rollback. The help texts describe the GPRS synchronisation: a command
is executed on the other phones when it arrives there ("Soldiers start
to flicker when you give them a command in the GPRS game. The command is
executed when it is delivered via GPRS to your allies and enemies", the
blue ring shows the delivery time). The phone that gives the order runs
it at once (0x2e200 queues it locally before sending), the others run it
when the bundle arrives and correct their state; this lives entirely in
the game. The server's part is to pass every command on once, to every
other player, in the order it arrived, without waiting (a bundle goes
out at the end of each incoming datagram).

### GPRS lobby (the server runs it)

Found after a hardware run on 2026-11-19 (Pathway to Glory, two
N-Gages): both phones stayed on "Waiting for side selection"
(`wait_for_server.txt`) for good. The GPRS lobby (6R72.APP 0x93024,
6R82.APP 0xa1050; the same code in both games) is not run by the host's
phone: every phone, the host included, asks the game server, and the
server hands out the player ids and the sides. In a Bluetooth game the
host's phone does that part (0x8eb3c, 0x8f17c). The server used to
pass type 0 on to the host, which ignores it, so nobody ever got the
type 9 the phones wait for.

| Type | Direction | Data                                                                                                      | Server                                                                                                                                                                                                                            |
| ---- | --------- | --------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0    | game      | u8 1 = has game settings (the host), 3 bytes garbage, u32 build                                           | answers 9; a client whose build differs from the host's gets status 6                                                                                                                                                             |
| 9    | server    | u32 player id, u8 1 = master, 3 bytes 0                                                                   | master = the room's creator, id 1; clients 2, 3, ... (as the Bluetooth host hands them out, 0x8e2ec/0x8ebb4; the id is the player id in the battle, see below)                                                                    |
| 14   | master    | game settings, 136 bytes                                                                                  | the server appends 52 bytes of its own settings (see below); to every player with an id, the master too (it waits for them, 0x934c4), and to later players right after their 9                                                    |
| 10   | game      | player record, 40 bytes: +0 id, +4 side (sent: the default side of the GPRS options), +8 reason, +20 name | fills in id, side and reason; sends the record to its player **first** (the game takes the first 10 as the answer and gives up if the id is not its own, 0x93720), then to the others, then the others' records to the new player |
| 37   | game      | u32 1 = takes the side, 0 = declined                                                                      | 0: the player is gone (12 to the others)                                                                                                                                                                                          |
| 11   | game      | u32 id, u32 side                                                                                          | a client may move only itself, the master anybody; at most 4 per side (like the BT host); to all with a record, the sender too                                                                                                    |
| 12   | game      | u32 id                                                                                                    | own id: leaving, to the others; the master with another id: removed, to everybody. Also sent by the server when a player leaves the room                                                                                          |
| 13   | master    | 12 bytes                                                                                                  | the start: to every player, the master too (0x93d04); the server keeps the first word and zeroes the rest (see below)                                                                                                             |
| 49   | server    | u32                                                                                                       | countdown (0x93d70); not sent                                                                                                                                                                                                     |

Every phone reads 0x88..0xb7 of the 14 it gets as well (6R72.APP
0x93528; Ikusa Islands 0xa157c up to 0xbb) into the GPRS settings
(0x130d18): the original server appended a block of its own to the
master's 136 bytes. Without it the phones read past the message
(hardware 2026-11-22: the briefing map closed at once instead of
waiting, then both phones hung on "Waiting for players" after loading).
The server now appends these 52 bytes (values marked "guessed" have no
default in the game to copy):

| Offset     | Size    | Into         | Meaning                                                                           | Server                                                 |
| ---------- | ------- | ------------ | --------------------------------------------------------------------------------- | ------------------------------------------------------ |
| 0x88..0x94 | 4 × u32 | +0x00..+0x0c | thresholds of five ranks (0xb1d8c)                                                | 1000, 2500, 5000, 10000 (guessed)                      |
| 0x98       | u8      | +0x10        | a per-player counter (+0x590), 1 outside GPRS                                     | 1                                                      |
| 0x9c       | u32     | +0x14        | command delay, replaces COMMAND_DEFAULT_DELAY (0x25980)                           | 90 (`COMMAND_DEFAULT_DELAY_GPRS` in the game's config) |
| 0xa0       | u32     | +0x18        | frames between clock pings (0xc3ecc)                                              | 300 (default outside GPRS)                             |
| 0xa4       | u32 s   | +0x20 (ms)   | screen countdown: briefing map, tag select (0xd0104, 0x132da0); 30 s outside GPRS | 60                                                     |
| 0xa8       | u32 s   | +0x24 (ms)   | results screen (0xc43d0)                                                          | 15 (guessed)                                           |
| 0xac, 0xb0 | u32     | +0x28, +0x2c | not read anywhere found                                                           | 0                                                      |
| 0xb4       | u16     | +0x30        | clock samples; the battle start sends this + 1 pings                              | 8 (the game's own default)                             |
| 0xb6       | u16     | +0x32        | clock samples kept                                                                | 16 (the game's own default)                            |
| 0xb8       | u32 s   | +0x34 (ms)   | Ikusa Islands only, not traced                                                    | 60 (guessed)                                           |

Reasons in +8 (0x97148, `globalbg_join_verification*.txt`): 0 "Joining
default side %1!", 1 "The default side is full. Join side %1 instead?",
2 "...To balance the forces join side %1 instead?", 3 "No players on side
%1, join this side instead?". The server keeps the default side unless
it has more players than the other side (reason 3 when the other side is
empty, else 2) or already 4 (reason 1). Sides are 0 and 1. Answering
"no" makes the game send 37 {0} and leave.

The 13 the master sends is only a request: {u32 number of players} and
eight bytes of whatever was on its stack (0x939d4). What the phones read
(0x93ca4) is {u32, u32 mission, u8, u8, u8 late join, u8 continue}: with
"continue" 1 the mission object takes the mission and the two bytes
from the 13 (0x6dce0 into +0x34, +0x0c, +0x10), otherwise it keeps the
start of the campaign file (`data/level/mp_campaigns/<name>.txt`,
`start_from`, 0xcdfc0); "late join" 1 skips emptying the queues 16, 26,
27, 31 and 51 after the first barrier (0x6e6c8). The server passes the
first word on and sends zeros for the rest.

### GPRS game: no phone is the host

Hardware run 2026-11-22: after the lobby the host showed the mission
map for a moment, then both phones hung on "Waiting host / Waiting
permanent damage data from the host". Traced through 6R72.APP; Ikusa
Islands has the same code (6R82.APP: lobby 0xa1050, barrier 0xd3b18,
clock ping 0xd3880, damage wait 0xe0f78).

The GPRS game (vtable 0x18d6e8 slots +0x18/+0x1c, 0x6dba0/0x6db80, both
into 0x6dbc0) gives every phone the part of a client: the sync object
is created with master 0 (0x6e5f4), the battle object with host 0
(0x6de34, `r10` from the player loop), and the mission object with the
GPRS flag +0x480 set (0xcdb48 with 1). Everything a host phone answers
in a Bluetooth game, the game server has to answer:

1. Lobby (0xccbb8 → 0x93024), see above. Ends with the 13.
2. Mission loop, while 0 ≤ mission < number of missions (0xd027c; a
   won battle moves one mission on, a lost one back, 0xd0248/0xd025c):
   1. Barrier (0x6e5f4, sync object mode 5 "init mission"; client
      0x131d58): drop old 15s, send 15 {1, …}, wait for as many 15s as
      it has connections (one). The master side (0x1317e0) would wait
      for a 15 from every connection and then answer each.
   2. Mission briefing and map (0xcf824). No network.
   3. Damage (0xd03e4, "Waiting permanent damage data from the host"):
      with the GPRS flag it first waits for a 35, then for a 41 (helper
      0x73250 polls the queue with Cancel), then takes a 23 if there is
      one (as the damage of this mission, +0x474[mission]). The phone
      makes the 35 itself behind a 34 and the 41 behind a 23 — only for
      a compressed or split message; a short plain 23 is queued as it is
      and makes no 41 (same as for 34).
   4. Records: the player list (class 0xddfec, slot 0x3c = 0xe1190)
      takes every queued 21 {u32 player id, data} and hands it to that
      player (slot 0x34). Nothing sends 21 in a GPRS game.
   5. Battle (0xc0134, mode 3, host 0; loop 0xc1e10):
      - Barrier again (sync object mode 0 "mission start").
      - Clock: 0xc3de8 sends N+1 type 31 pings (N from the settings
        word at +0x30) and waits until all are answered; afterwards
        0xc3ecc sends one every few hundred frames. A ping is {u32
        sent, u32 host received, u32 host sent, u32, u16, u16}; the
        host fills in words 1 and 2 with its clock (0xc3f80), the
        client works out the offset (0x13affc) and from then on runs
        on the host's clock (0xc3e78).
      - Barrier with start time (0xc336c, client part 0xc3644): drop
        old 15s, send 15, wait for one; its second word is the start
        time in the host's clock (+0x94), the battle starts when the
        clock gets there (0xc20e8).
      - Commands (16, bundled as 32), Field Radio (43) and the rest go
        to the other players; remote players read 16, 39 (host ended,
        +0x25) and 27 (player ended, +0x24) from the connection
        (0x27750). 36 {u32} would pause the game (0xc3aec), sent only
        by a host.
      - At the end 27 {u32} to everybody (0xc2b94).
   6. After the battle, if the campaign goes on: barrier (0x6e530), then
      game slot 0x28 (0x6e03c). Nothing in the GPRS path saves the
      damage of the battle or sends a 23.

Player ids. The battle uses the lobby's player id as its own: command
29 (sent by every phone at the end of the loading screen, builder
0x26da4) carries the sender's id (player slot 0x6c) and the receiver
looks the player up with it (0x24aa4, world slot 0x19c). The server
first handed out 0 for the master and 1, 2, ... for the clients; on
hardware (2026-11-22) the host crashed with exception 12 three seconds
after the client's command 29 arrived, near the end of the loading
screen. The Bluetooth host starts at 1 (its own id, 0x8e2ec) and gives
2, 3, ... to the clients, so 0 is no player; the server now does the
same.

What the server does (gcs_ptg.c, both games):

- 15: barrier over all players with a player id; when the last one has
  sent its 15, every player gets 15 {1, server ms + 500}, an empty 34 and
  an empty 23 (both compressed), so the damage wait finds its 35 and 41
  whatever barrier comes before it. Extra empty ones stay queued and do
  no harm (each mission takes one 35, one 41 and at most one 23). A
  player leaving the room completes a barrier that waited for it.
- 31: back to the sender with the server's milliseconds in words 1 and 2.
- 13: first word of the master's request, the rest zeros.
- Everything else from a client now goes to all other players instead
  of to the host phone.

Not covered: permanent damage carried from one mission to the next (the
server would have to keep what the game never sends), 21 records,
the pause (36), and whatever the original server did with ranked
results.

### Pathway to Glory (the first game)

Pathway to Glory v1.03 (6R72, game class 43520 = 0xaa00) has the same
GPRS mode on an older Arena stack: ArenaFramework 2.01.054 (the game
creates it with ordinal 154), snapcomm "Nov 11 2004" (Thumb; delivers
op 15 like the Ikusa Islands build), `arena.amr` with
`im.ptg.redlynx.sf.yav4.com`, `GameServerFQDN`
snap.ptg.redlynx.sf.yav4.com and retrieval43520 / reporter43520.
`game.rsc` has the same settings with other battlegrounds (S. Europe,
France, Rhein, Sicily and their numbered maps). The Field Radio, the GPRS
help texts and the server error list are the same.

The network layer (6R72.APP 0x78d80, receive 0x79884, send 0x79248 via
CArenaFramework slot 0x3c, keep alive 0x7977c) has the same message
types, pieces, zlib messages, bundles (32 carries 16; 34 carries 21 and is followed by its own
35; 23 is followed by its own 41), acknowledgements and keep alive, with these
differences:

- The whole first header byte is the type; above 51 the game treats the
  message as an error. There is no restart bit (no drop flag either).
- The queue number is the whole header with bit 15 removed, so the tag
  bits must be 0 (the acknowledgement still copies them).
- Type 50 always ends the connection, whatever the code.

The server registers both games (`gcs_ptg_game()`, `gcs_ptg1_game()`)
and leaves out the restart bit and the tag for 43520. Its lobbies are
also `Freestyle,Ranked`; gcs_snap.c now keeps lobbies per game class, so
that Worms World Party and the two Pathway to Glory games each get their
own "Freestyle" and "Ranked" (before, the first game to register owned
the name and the others saw no lobby).

### Assumptions to check on hardware

- Teams: the host's Bluetooth code sends the Field Radio only to the
  players of the side whose turn it is; the server does not know the
  sides and sends it to all other players.
- GPRS game: whether the original server sent damage data (23) or
  records (21) of its own; the number of barriers per battle (the
  server does not count them); whether its clock was in milliseconds.
- GPRS lobby: whether the original server balanced the sides the same
  way, what +0xd0 bit 2 of the settings means (without it the game does
  not wait for its own record), when it sent the countdown 49, and
  whether the sides are really 0 and 1.
- Whether a game ever sends more than one SNAP message can carry
  (~1000 bytes) in one call; the server takes several messages per
  datagram but does not join SNAP packet series (the 0x2000 kind).
- The receive path of op 15 reliable (arenabase slot 0x50) up to the
  game's observer slot 0x34; op 16 would be the alternative.

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
(op 17 in, op 16 out, gcs_snap.c). Answers must be op 16 (private data:
u32 count, count x u32 recipient uid, then the game message); the server
sends {1, uid}. Pocket Kingdom's snapcomm.dll (dispatch table 0x4338,
index = op - 3) handles incoming op 16 at 0x49ec (reliable: observer
event 0x13, then ArenaFramework 0xab3c, then GameDataReceived) and drops
op 17 (default case 0x517c). Op 15 is not dropped by snapcomm: 0x4910
raises event 0x12 (reliable) or 0x14 (unreliable), ArenaFramework takes
it in 0xaae4 / 0xab94 (vtable 0x167cc, slots 0x50 / 0x58; the op 16
twins are 0xab3c / 0xabec) and, while a game is attached (AF+0x994),
hands it to the game observer's slot 0x34 instead of 0x38. In 6RAH that
slot is an empty function (vtable 0xb0c20, thunk 0x7d75c -> 0x67c2c
`bx lr`), so the game throws op 15 data away. Without a game attached
both ops go to the object at AF+0x8e0 (slots 0x50 / 0x54). Until 2026-11-16 the server answered with op 15, so the
game never saw 129, 130 and the other answers; the Global Auction waits for the 130 and timed
out with trigger039 before ArenaFramework was even asked to switch rooms.
Battles are not played against
stored kingdoms, as the manual suggested: the defender's game is online
and answers scouting and attacks itself; the server passes the messages
on. Server code: `server/src/gcs_pk.c`, tests: `tests/server/test_pk.c`.

### Game class and rooms

game.rsc sets three flags (keys 0x2007, 0x2010, 0x201c) and "Scrimmage"
as lobby and room name (keys 0x200a, 0x200b); it has no class number.
The SegaChat presence carries `GameClassId='1'` on hardware, so the server
attaches the game server to class 1 (`GCS_PK_GAME_CLASS`) and lists the
lobby "Scrimmage". Not verified: the class the SNAP login carries (the
server also scans the login for it).

After entering the lobby, the Play Now screens (caamplaynow.dll, 0x94bc)
look for the room "Scrimmage" (RoomToJoin) in the room list and enter
it. Without it they log "room not found or not available" and go back
to the mode selection; Pocket Kingdom has no CreateRoomToJoin, so it never
creates the room. The server therefore keeps fixed rooms for a game
(`gcs_snap_game.rooms`, kept when empty, up to 64 users, entries
`room[@lobby][#status]`). Pocket Kingdom has three lobbies with one room
each:

| Lobby / room | Status     | Used by                                                                                                                                                                                                                                                                  |
| ------------ | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Scrimmage    | 1          | Play Now "Scrimmage" (RoomToJoin)                                                                                                                                                                                                                                        |
| Auction      | 1          | the Global Auction: the game switches with CArenaFramework slot 0x6c (lobby "Auction", room "Auction"); see "Global Auction: the room switch" below                                                                                                                      |
| Ranked       | 0x30000001 | Play Now "Ranked": SelectGameRooms over all lobbies (op 73) with STAT & 0x30000000 = 0x30000000, USER < 0x7fff, ARNA..ARND = 0; the game may not create a room (ProhibitRoomCreation: "no room was found, and ProhibitRoomCreation is set!", back to the mode selection) |

The room entries had the wrong layout at first (the room id where
ArenaFramework expects the number of users, so every room looked full);
see op 11 above. The game server only lets
kingdoms of the same room meet (search, scout, attack), so Scrimmage
and Ranked stay apart; chat goes everywhere. Game data from a user who has not
entered a room yet reaches the game server, too.

### Global Auction: the room switch

From arenaframework.dll 0xccc8 (slot 0x6c), arenabase.dll and
snapcomm.dll (static analysis, 2026-11-16). The game calls slot 0x6c
(lobby "Auction", room "Auction", 0) after its keep alive (type 2, answered by 130) and
then waits for observer slot 0x44 (0 = done). ArenaFramework keeps a
state at +0x1304 and goes through these requests, each one started by
the answer to the one before:

1. op 9 (0xb000) {current lobby, NAME}: the name of the current lobby
   (+0xa58). It is only known this way, so this request always comes
   first. If it is the target lobby, step 2 is skipped.
2. op 72 {0, 1, NAME 0x41 "Auction"}: lobby search. Exactly one entry is
   needed; its id (+32) is the target lobby. No entry: error 0x2b09,
   more than one: 0x2b0a.
3. op 73 {0, 2, NAME 0x41 "Auction", LOID 1 lobby}: room search; the
   room id is at +36 of the first 40 byte entry (none: 0x2b07, more than
   one: 0x2b08).
4. leave the room (op 7), leave the lobby (op 7, 0xb000), enter the
   target lobby (op 6, 0xb000), enter the room (op 6), as far as needed.
5. observer slot 0x44 with 0; any error code ends the switch, and the
   game shows its connection error.

The server used to answer op 72 as a room search in the user's lobby, so
the lobby "Auction" was never found and the Global Auction failed after
the keep alive. Error codes 0x2b01..0x2b05 come straight from slot 0x6c
(no ArenaBase, no SNAP connection, name too long, not in a lobby, busy),
0x2b06 is a failed answer.

### Messages

Byte 0 is the type, byte 1 a sequence number the answer echoes (not in
type 3). Names are 16 byte fields (15 characters, zero padded). Numbers
in auction messages are big endian.

|   Type |    Len | Direction      | Content                                                                                                                                                                                             |
| -----: | -----: | -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
|      1 |     24 | game to server | online: [2] area, [3..6] kingdom look, name @8; answer 129 (4 bytes, only counted)                                                                                                                  |
|      2 |     20 | game to server | keep alive (every ~165 frames), name @4; answer 130                                                                                                                                                 |
|      3 |     20 | game to server | going offline, name @4 (no answer)                                                                                                                                                                  |
|      5 |     20 | game to server | moved: [2] new area, name @4 (no answer)                                                                                                                                                            |
|      6 |     20 | game to server | enemy search: [2] area, name @4; answer 134                                                                                                                                                         |
|    134 |    104 | server to game | up to 5 entries of 20 bytes from offset 4: name[16], look[4] (the castle, transporter, mark and colour other players see); empty name = end                                                         |
|      7 |     36 | relayed        | scout: from @4, to @20                                                                                                                                                                              |
|    135 |     52 | relayed        | scout answer from the defender's game: [2] result, to @4, from @20, party data                                                                                                                      |
|      9 |     88 | relayed        | attack: [3] party, from @4, to @20, 4 units x 12 bytes from @40                                                                                                                                     |
|    137 |     88 | relayed        | attack answer from the defender's game: [2] result, to @8, from @24, defending units                                                                                                                |
|     10 |     36 | relayed        | the battle starts: from @4, to @20; the server also sends it back to the attacker                                                                                                                   |
|     18 |    116 | relayed        | chat: [2] area, from @4, to @20, text @36 (78 chars), [115]; answer 146: [2] = 0 delivered (or kept), anything else makes the game report trigger037 "Couldn't deliver the message!"                |
|     13 |     32 | game to server | exhibit: name @4, item @20..26, starting price u32 @28; answer 141                                                                                                                                  |
|    141 |      8 | server to game | [3] 0 exhibited (trigger041), 1 too many (040), 2 refused (043); u32 @4 auction id                                                                                                                  |
|     14 |     32 | game to server | browse: [2] category, [3] item type, [4] rarity, [8] sub-filter (0xff = any), u32 @12 page, name @16; answer 142                                                                                    |
|    142 |    248 | server to game | u16 @2 page, u32 @4 matches (0: trigger052), 10 entries from @8                                                                                                                                     |
|     15 |     28 | game to server | bid: u32 @4 auction, u32 @8 amount, name @12; answer 143                                                                                                                                            |
|    143 |      8 | server to game | [2] 0 placed (044), 1 someone bid higher (045), 2 ended (046), 3 too many bids (042), 4 own exhibit (050), 5 refused (051); u32 @4 the bidder's previous bid on it: the game pays amount minus this |
|     16 |     20 | game to server | own list: [3] mode 0 exhibits, 1 bids (looking), 2 exhibits, 3 bids (collecting), name @4; answer 144                                                                                               |
|    144 |    244 | server to game | 10 entries from @4                                                                                                                                                                                  |
|     21 |    112 | game to server | kingdom summary: u32 @4 victory points, u32 @8 loot, unit table, parties, look, area; answer 149                                                                                                    |
| 17, 22 | 20, 32 | game to server | built by the game, never sent (answers would be 145, 150)                                                                                                                                           |
|    255 |      4 | server to game | the game goes offline at once (never sent by this server)                                                                                                                                           |

Auction list entry (24 bytes): u32 auction id, [4] status (0 = end of
list), [5..11] the exhibit's item bytes 20..26 (category, type, item,
three item values, level), u32 @12 seconds left (0: ended, the time is
not shown), u32 @16 price (highest bid or starting price), u32 @20 the
player's own bid.

| List                      | 1                | 2                           | 3                                   | 4                            |
| ------------------------- | ---------------- | --------------------------- | ----------------------------------- | ---------------------------- |
| browse (142)              | -                | the player has the high bid |                                     |                              |
| exhibits (144, modes 0/2) | running, no bids | running, bids               | sold: the game adds @16 to the loot | no sale: the item comes back |
| bids (144, modes 1/3)     | outbid           | high bid                    | won: the game adds the item         | lost: the game adds @20 back |

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
- Message 10 also goes back to the attacker. The game's handler
  (6RAH 0x66de0) compares the name @4 with its own: someone else's 10
  starts the defence, its own 10 confirms the attack (it clears the
  pending attack with the 10's sequence number; the 137 handler 0x66698
  moves the attack to that number when it sends the 10). Without the echo
  the attack stays pending and the mission check (0x28760) reports "It
  seems X has moved Area!" (mestext.dat records 322/323; 319 is "False
  alarm", 320/321 "nobody home" for result 255), while the defender's
  game plays the battle.
- Chat to a known kingdom that is offline is kept (at most 50 per player)
  and delivered after its next message 1; unknown names get 146 with 1.
  Byte 2 of 146 is a failure flag: the game's handler (6RAH 0x6817c)
  ignores 0 and for any other value starts a mission (type 11) that ends
  with trigger037 "Couldn't deliver the message!" (0x70e6c). The server
  used to send 1 for a delivered message, so every message looked
  undelivered.
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

| DLL                | Loaded by                                                                                | How                                                                                             |
| ------------------ | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| SnapComm.dll       | arenabase                                                                                | RLibrary::Load from its own `\System\Libs`, UID3 0x101fde38, ordinal 1 = NewLC (arenabase pops) |
| sc_lib.dll         | arenabase (dynamic, UID3 0x101fde44), arenaframework + cafmjabber (import ordinal 3)     | 1 NewLC (calls slot 0x0c), 2 returns 0, 3 NewL                                                  |
| HttpDllRequest.dll | arenaframework, arenabase, cafmlogin: load by name (fallback HttpRequest.dll), ordinal 1 | non-leaving new, returns the object                                                             |

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
- Callbacks: a table of observer objects at handle+0x554, indexed by id. Each id calls a different virtual slot of the observer with (header, parsed struct). arenabase registers ids 0..20 during login (0x796..0xae2).
- Transport of the original: one UDP socket (its own BSD socket layer over ESOCK), default port apparently 8080.

### sc_lib client (vtable 26 slots)

| Slot             | SegaChat request                                                                                                   |
| ---------------- | ------------------------------------------------------------------------------------------------------------------ |
| 0x08             | destructor                                                                                                         |
| 0x0c             | ConstructL                                                                                                         |
| 0x10             | connect / login                                                                                                    |
| 0x14             | startup, then segachat_send_event                                                                                  |
| 0x18, 0x1c, 0x40 | segachat_send_event (event_type, game_class_id, match_id, snap_name, client_time); probably reporter/match results |
| 0x24             | get roster                                                                                                         |
| 0x28             | message (chat, groupchat, headline)                                                                                |
| 0x2c             | presence                                                                                                           |
| 0x30             | buddy accept/reject                                                                                                |
| 0x38, 0x3c       | roster item, remove                                                                                                |
| 0x44             | segachat_retrieve_req (rankings)                                                                                   |
| 0x48             | browse jid                                                                                                         |
| 0x4c..0x60       | offline messages: count, headers, fetch, purge                                                                     |
| 0x64             | buddy request (subscribe)                                                                                          |
| 0x68             | data message                                                                                                       |
| 0x6c             | service request                                                                                                    |

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

Pocket Kingdom:

1. Build the four DLLs with the N-Gage toolchain and fix what the
   compiler finds.
2. Run Pocket Kingdom with them and collect `PlArena.log`, together with
   the server log at level 3 (shows every GCA message).
3. Identify the remaining slots from the logged calls and the arenabase
   wrappers. Implement the SnapComm observer events (login result first,
   then lobby/room lists and entries), forward the HTTP slots as HTTP_REQ.
4. The All-Time Owner Board (retrieval board `player_skills`).

Every ArenaFramework game (Communicate and Play menus):

1. Friends: add a friend from a second N-Gage, authorise, chat, remove;
   the server log shows every presence and roster push at level 3.
2. Play > Rankings, Tournaments, Activities, Games: check that the lists
   show their labels and that choosing an item opens the right page
   (the parameter names come from cafmplay.dll; the screen that sends
   `cmd=search` is assumed to be "Find players").
