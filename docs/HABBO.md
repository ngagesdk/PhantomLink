# Habbo Islands: Habbo Console

Habbo Islands (Sulake, 2006, app `6R83`, UID `0x101FD41E`) is not an
N-Gage Arena game and does not use Bluetooth multiplayer. Its only online
feature is the **Habbo Console**, the messenger of Habbo Hotel: log in with
a Habbo account, see your friends (online or their last visit), read and
send messages, accept or reject friend requests, find a Habbo and ask them
to be your friend, remove a friend. The islands themselves stay offline.

PhantomLink serves the console from the same server as the Arena games,
with the same accounts.

## Setup

The game connects over an access point of the phone, like the Arena games,
so both Arena routes work for it. The PhantomLink app writes the host name
of the console, `builder.sulake.com`, into `C:\System\Data\hosts` together
with the Arena names.

**Over GPRS** (no bridge):

1. Server: `gamecommsd` with `direct = 1`. The console uses TCP port 8080
   (`direct_habbo_port`, default 8080; open or forward it like the Arena
   ports).
2. PhantomLink app: Settings > **Arena server**, then **Arena over GPRS**,
   restart the N-Gage and choose **Arena over GPRS** once more until it
   reports "Arena names: OK" (docs/ARENA.md, "Arena over GPRS").
3. Habbo Islands: **Set Access Point** (in the console's options): the
   operator's packet data access point. Then open the Habbo Console, pick the server
   "PhantomLink" and log in.

**Over Bluetooth** (PPP to a bridge): **Arena over BT** in the PhantomLink
app (docs/ARENA.md), restart, and choose the access point `PhantomLink`
in the game. The bridge passes port 8080 to the server like the Arena
ports.

**Accounts**: the shared PhantomLink accounts (docs/ARENA.md, "Accounts").
An Arena account works in the console with the same name and password; a
new name creates the account on the first login. The game sends no IMEI,
so a console login neither checks nor makes the IMEI binding (the binding
is made by the first Arena login). Players find each other by account
name. The Arena profile's quote is shown as the friend's text.

**Do not log in before the hosts file is in force** (the check in the
app). `builder.sulake.com` is a real Sulake domain; without the redirect
the game would try to reach it.

Server data: `habbo_data` (default `gamecomms-habbo.dat`): console users,
friendships, open friend requests and unread messages. Empty: memory only.

Limits (`server/src/gcs_habbo.h`): 200 friends per user, 100 unread
messages per user, 255 bytes per message (UTF-8), 30 messages per update
(the rest comes with the next update). A session ends after an hour
without requests (the game logs in again on its own); a user counts as
online for 15 minutes after a console request, or until logging out.

## Protocol

From the console code in the game's `SULAKEUI.DLL` (state machine
0x26800-0x29200) and `SULAKENETWORK.DLL` (HTTP client with its own cookie
jar), retail version 1.0.7 of 2006-03-24. The game brings the Symbian HTTP
framework in its own `system\libs`, because the N-Gage firmware has none.

HTTP/1.1, `User-Agent: Habbo Console (1.0)`, `Accept: */*`.

1. **Server list**: `GET http://builder.sulake.com:8080/test/locator.html`.
   Text, one server per line (CRLF or LF), fields separated by `;`:
   `id;display name;base URL`. The name goes into the server list, the base
   URL is the prefix of the servlets. PhantomLink answers
   `1;PhantomLink;http://<Host header>/habbo/`, so the game stays on the
   same host and port.
2. **Login**: `POST <base>AuthenticationServlet`,
   `Content-Type: application/x-www-form-urlencoded`, body
   `user=NAME&pass=PASSWORD`. `0` is success; anything else shows "You have
   entered the wrong password or Habbo name!". The session is a cookie:
   the game keeps the `Set-Cookie` pairs and sends them back as `Cookie`.
3. **Messenger**: `POST <base>MessengerServlet`, body
   `m=ID[&p1=...][&p2=...]`. `m` is the **FUSE client message ID** of Habbo
   Hotel's own messenger (FUSE 0.2.0, Habbo v16):

| m  | FUSE name              | Parameters                         | Answer on success |
|----|------------------------|------------------------------------|-------------------|
| 12 | MESSENGERINIT          | -                                  | lists, full       |
| 15 | MESSENGER_UPDATE       | -                                  | lists, update     |
| 32 | MESSENGER_MARKREAD     | `p1=<message id>`, repeated        | `0`               |
| 33 | MESSENGER_SENDMSG      | `p1=<friend id>&p2=<text>`         | `0`               |
| 37 | MESSENGER_ACCEPTBUDDY  | `p1=<user id>`                     | friend record     |
| 38 | MESSENGER_DECLINEBUDDY | `p1=<user id>`                     | `0`               |
| 39 | MESSENGER_REQUESTBUDDY | `p1=<user id>` (FUSE: a name)      | `0`               |
| 40 | MESSENGER_REMOVEBUDDY  | `p1=<user id>`                     | `0`               |
| 41 | FINDUSER               | `p1=<name>`                        | user record       |
| 0  | (logout)               | -                                  | `0`               |

Every answer is first compared with `2` (not logged in: the game logs in
again and repeats the request, except for the logout) and `1` (this
request failed: "Failed to send message!", "No Habbo found!", ...).

The other answers are binary, **not** in Habbo's FUSE encoding (no Base64
headers, no VL64, no chr 2 / chr 1): Java `DataOutputStream` style,
big-endian. `long` is 8 bytes, `str` is a u16 length followed by UTF-8
bytes (`writeUTF`):

- user / friend: `long id, str name, str text, u8 online, [str last visit
  if online == 0]` (at least 13 bytes; an online friend shows "Currently
  online")
- message: `long id, long sender id, str sender name, str text, str date
  and time` (at least 22 bytes)
- friend request: `long user id, str name` (at least 10 bytes)
- lists (m=12): `str (ignored), u16 n + n friends, u16 n + n messages,
  u16 n + n requests, [u16 maximum message length]`. m=15 is the same
  without the first `str`; the game replaces friends by id but appends
  messages and requests, so an update carries each of them only once.

A length that does not fit gives "Syntax error in server response!".

### Shared with Habbo Hotel

The message IDs and their meaning and the accounts: the original servlets
were an HTTP gateway into the hotel's FUSE messenger. Transport, encoding
and framing differ. A bridge to a FUSE v16 retro server could map the IDs
one to one; PhantomLink keeps the console users among themselves.

## Parts

| Part | Where | Status |
|---|---|---|
| Console back end | `server/src/gcs_habbo.c` | written, tested |
| Port 8080 for bridges and direct clients | `server/src/gcs_native.c`, `gcs_direct.h`, `gcs_main.c` (`direct_habbo_port`) | written, tested |
| Hosts file entry | `app/src/ArenaSetup.cpp` | written, not built here |
| Tests | `tests/server/test_habbo.c` (answers decoded like the game, requests over TCP through gcs_direct) | passing |

Not yet verified on hardware: whether the game lists the `PhantomLink`
(GSM data) access point next to packet data ones, the character set the
game expects for names (UTF-8 is converted on its side), and whether it
URL-encodes the login fields (the server decodes them either way).
