# Compatibility

One replacement GAMECOMMS.DLL is built for each original variant
(`build/dll/out/vN/gamecomms.dll`). Each has the original UIDs, the same
number of exports, and every export number (ordinal) points to an
implementation of the original function. The export tables are defined
in `dll/variants/variants.py` and checked against the original DLLs by
`tests/compat/check_variants.py`.

Confidence:

- V: verified from the original code
- S: behaviour verified, name taken from the export order
- I: inferred

Status:

- Full: implemented over the network with the original behaviour
- Inert: present and safe to call, but without function (see below)

## Variants

The list of games and which DLL each one needs is in the
[README](../README.md#which-dll-for-which-game).

| Variant | md5sum of the original | Version tag | API | Ordinals |
|---|---|---|---|---|
| v0 | cb326c500bdb795494d55d30196b8711 | SDK 1.0 beta | 1.x | 33 |
| v1 | 79c25cdd1972c287712475ee544c7bf3 | none | 1.x | 33 |
| v2 | 8863fdea897f1c63807ad69252a3566f | none | 2.x | 55 |
| v3 | f6fbc3c745ae0ecf584b3ef7ec4371ef | none | 2.x | 56 |
| v4 | c2f184012cd0635f3065b7d5042702ce | SDK v1.0 | 2.x | 56 |
| v5 | e8d317204a763d71215680d567eb71e0 | SDK v2.6 | 2.x | 57 |
| v6 | bb02377c5361597f8d8503f5de584845 | SDK v2.5 | 2.x | 57 |
| v7 | f07f48ee44cca60046f40ce41858a160 | SDK v2.5 | 2.x | 57 |
| v8 | d4c10fcebfb6da8da6b757aa8392b6d7 | SDK v2.0 | 2.x | 57 |
| v9 | 6ac8b1a1e490b367a1d5e46bb720fe70 | SDK v1.0 | 2.x | 57 |
| v10 | 827df54717bc210daa96aef9e12de4a3 | SDK v3.0 | 2.x | 63 |
| v11 | 75536306ef48b1928b4562890d01c9c6 | SDK v3.1 | 2.x | 63 |

v0 is the SDK beta and was not shipped with a game, so no replacement is
built for it. v5, v6 and v7 contain identical code. They are still built
as separate files, so every game gets exactly the export table it was
linked against.

Install the replacement built for the game's variant. Using a different
variant will not work, because v1 games use a different API than v2 to
v11.

## GameComms 1.x (v1)

| Ordinal | Function | Conf. | Status | Notes |
|---|---|---|---|---|
| 1 | RSGEDebugLog::RSGEDebugLog() | V | Full | 20 byte layout kept |
| 2 | CGameBTComms::ConnectState() | V | Full | |
| 3 | CGameBTComms::ConnectionRole() | V | Full | |
| 4 | CGameBTComms::ContinueMultiPlayerGame() | S | Full | continues when the host and all clients asked |
| 5 | CGameBTComms::DisconnectClient(TUint16) | V | Full | KErrNotReady unless host, KErrBadHandle for 0 |
| 6 | CGameBTComms::Disconnect() | V | Full | while paused: temporary, ReconnectL restores |
| 7 | CGameBTComms::EndMultiPlayerGame() | S | Full | |
| 8 | CGameBTComms::GameState() | V | Full | |
| 9 | CGameBTComms::GetLocalDeviceName(THostName&) | V | Full | DeviceName from GameComms.ini, else the Bluetooth name |
| 10 | CGameBTComms::IsShowingDeviceSelectDlg() | S | Full | true while the bridge selection or the host list is shown |
| 11 | CGameBTComms::NewL(MGameBTCommsNotify*, TUint32, RSGEDebugLog*) | V | Full | |
| 12, 13, 22 to 30 | RSGEDebugLog::NewLine, Open, WriteLine, Write | V/S | Full | return KErrNone, as in the original |
| 14 | CGameBTComms::PauseMultiPlayerGame() | S | Full | |
| 15 | CGameBTComms::ReconnectL(TBool) | S | Full | |
| 16 | CGameBTComms::SendDataToAllClients(TDesC8&) | V | Full | |
| 17 | CGameBTComms::SendDataToClient(TUint16, TDesC8&) | V | Full | |
| 18 | CGameBTComms::SendDataToHost(TDesC8&) | V | Full | |
| 19 | RSGEDebugLog::SetLoggingLevel(TLogLevel) | V | Full | |
| 20 | CGameBTComms::StartClientL() | V | Full | shows the host list instead of the Bluetooth device dialog |
| 21 | CGameBTComms::StartHostL(TUint16, TUint16) | V | Full | |
| 31 | CGameBTComms::~CGameBTComms() | V | Full | |
| 32 | RSGEDebugLog::~RSGEDebugLog() | V | Full | |
| 33 | RSGEDebugLog::Close() | V | Full | |

All 12 callbacks of MGameBTCommsNotify are delivered asynchronously with
the documented arguments and error codes.

## GameComms 2.x (v2 to v11)

The "Variants" column lists which variants have the export.

| Ordinal | Function | Conf. | Status | Variants |
|---|---|---|---|---|
| 1 | RSGEDebugLog::RSGEDebugLog() | V | Full | v2 to v11 |
| 2 | BluetoothPowerState(TBool&) | V | Full | v2 to v11 |
| 3 | CGameBTDiscoverer::Cancel() | V | Full | v2 to v11 |
| 4 | CGameBTDeviceList::Reset() | V | Inert | v2 to v11 |
| 5 | RSGEDebugLog::Close() | V | Full | v2 to v11 |
| 6 | ConnectState() | V | Full | v2 to v11 |
| 7 | Connect() | V | Full | v2 to v11 |
| 8 | Connect(TUint16) | V | Full | v2 to v11 |
| 9 | ConnectionRole() | V | Full | v2 to v11 |
| 10 | CGameBTDeviceList::Count() | V | Inert | v2 to v11 |
| 11 | DeclareRemoteDevice(const TDeviceDetails&) | V | Full | v2 to v11 |
| 12 | CGameBTDeviceList::Delete(TInt) | V | Inert | v2 to v11 |
| 13 | CGameBTDiscoverer::DeviceClass() | V | Full | v2 to v11 |
| 14 | Disconnect() | V | Full | v2 to v11 |
| 15 | Disconnect(TUint16, TBool) | V | Full | v2 to v11 |
| 16 | CGameBTResolver::Start(const TDeviceDetails&) | V | Full | v2 to v11 |
| 17 | CGameBTDeviceList::CurrentItem() | V | Inert | v2 to v11 |
| 18 | CGameBTDeviceList::ListBox() | V | Inert | v2 to v11 |
| 19 | CGameBTDeviceList::At(TInt) | V | Inert | v2 to v11 |
| 20 | GetLocalDeviceName(THostName&) | V | Full | v2 to v11 |
| 21 | CGameBTDeviceList::Hide() | V | Inert | v2 to v11 |
| 22 | HostAcceptMode() | V | Full | v2 to v11 |
| 23 | CGameBTDeviceList::InsertL(TInt, const TDeviceDetails&) | V | Inert | v2 to v11 |
| 24 | CGameBTDiscoverer::IsIdle() | V | Full | v2 to v11 |
| 25 | CGameBTResolver::NewL(...) | V | Full | v2 to v11 |
| 26 | CGameBTDeviceList::NewL(...) | V | Inert | v2 to v11 |
| 27 | NewL(MGameBTCommsNotify*, TUint32, RSGEDebugLog*) | V | Full | v2 to v11 |
| 28 | CGameBTDiscoverer::NewL(...) | V | Full | v2 to v11 |
| 29, 30, 41 to 49 | RSGEDebugLog::NewLine, Open, WriteLine, Write | V/S | Full | v2 to v11 |
| 31 | CGameBTDiscoverer::StartL() | V | Full | v2 to v11 |
| 32 | SendData(const TDesC8&) | V | Full | v2 to v11 |
| 33 | SendData(TUint16, const TDesC8&) | V | Full | v2 to v11 |
| 34 | SetBluetoothPowerState(TBool) | V | Full | v2 to v11 |
| 35 | CGameBTDiscoverer::SetDeviceClass(const TBTDeviceClass&) | V | Full | v2 to v11 |
| 36 | SetHostAcceptMode(TAcceptMode) | V | Full | v2 to v11 |
| 37 | RSGEDebugLog::SetLoggingLevel() | V | Full | v2 to v11 |
| 38 | CGameBTDeviceList::Show() | V | Inert | v2 to v11 |
| 39 | StartClientL(TInt, CEikAppUi*) | V | Full | v2 to v11 |
| 40 | StartHostL(TInt, CEikAppUi*) | V | Full | v2 to v11 |
| 50 | CGameBTResolver destructor | V | Full | v2 to v11 |
| 51 | CGameBTDeviceList destructor | V | Inert | v2 to v11 |
| 52 | ~CGameBTComms() | V | Full | v2 to v11 |
| 53 | RSGEDebugLog destructor | V | Full | v2 to v11 |
| 54 | CGameBTDiscoverer destructor | V | Full | v2 to v11 |
| 55 | CGameBTDeviceList::AppendL(const TDeviceDetails&) | V | Inert | v2 to v11 |
| 56 | SetLocalDeviceName(THostName) | V | Full | v3 to v11 |
| 57 | CGameBTDiscoverer::StartFullL() | V | Full | v5 to v11 |
| 58 | EndSession() | V | Full | v10, v11 |
| 59 | IsDiscoverabilityModeLimited(TBool&) | S | Full (1) | v10, v11 |
| 60 | visibility getter (TBool&) | S | Full (1) | v10, v11 |
| 61 | SetDiscoverabilityModeLimited(TBool) | V | Full (1) | v10, v11 |
| 62 | discoverer search mode setter | I | Full (1) | v10, v11 |
| 63 | visibility setter (TBool) | S | Full (1) | v10, v11 |

(1) Returns KErrNone. Bluetooth discoverability has no meaning when the
sessions run over the network. The getters 59 and 60 answer "not
limited" and "visible"; the original writes the TBool& as well, and
games read it without checking the return value.

### Helper objects

The device list, discoverer and resolver are parts of the original library
that were exported as well. Some games use them for their own host search
instead of the built-in list (Ashen does).

- Discoverer: the original searched for nearby phones by Bluetooth. The
  replacement asks the server for the open games of the same title and
  reports each one to the game's observer as a "device" named after the
  host, followed by the final KErrEof report, as the original did. Hosts
  that do not accept players (accept mode 2) are not reported.
- Resolver: the original checked a device's Bluetooth service records for
  the game. The replacement answers KErrNone for entries from the
  discoverer and KErrNotFound for anything else.
- Device list: an inert stand-in. It keeps its items (AppendL, InsertL,
  At, Count, Delete and Reset work) but shows nothing on screen. If a game
  turns out to display this list, it has to be implemented as well.

### Differences to the original on the network

| Original (Bluetooth) | Replacement (network) |
|---|---|
| The client finds hosts by Bluetooth inquiry or the game's own UI | The built-in list shows open sessions of the same game. DeclareRemoteDevice with any other device joins the first open session of the same game. |
| TDeviceDetails holds a Bluetooth address | It holds a made-up address starting with 'G' 'C' plus the session or client id, and the player name |
| Host accept modes 0, 1, 2 through the Bluetooth service record | The session is listed (modes 0 and 1) or closed (mode 2) on the server. Confirmation happens on the host. |
| Connect on the client registers with the host, Connect on the host connects back | Same sequence: client Connect joins, host Connect sends CONNECT, both report Connected |
| KErrIncompatibleGame from the Bluetooth service check | From the server's game UID check |
| BluetoothPowerState and SetBluetoothPowerState | Report "on" and succeed. Bluetooth has to be on for the bridge anyway. |
| Panic GC-MAIN 4 when Bluetooth is off | Not raised. A missing bridge is reported through the normal error callbacks. |

## Why games fail

Found by analysing the failing games listed in the README and the Rifts
source code (November 2026). `python tools/gc_usage.py <game folder>`
prints the same information for any game.

### 1. The game does not use GameComms for multiplayer

These games import only the Bluetooth power and name functions
(ordinals 2, 27, 34, sometimes 56). Their multiplayer runs on their own
RFCOMM sockets, so a GAMECOMMS.DLL replacement never sees any of it. The
host search lists the bridge as a phone, the connection to it fails and
the game stops in matchmaking. They cannot be fixed inside the DLL.

| Game | Own stack |
|---|---|
| Pathway to Glory, Pathway to Glory: Ikusa Islands | inquiry by name, RFCOMM channel 4, no SDP |
| Colin McRae Rally 2005 | own sockets (6R66_2.APP), GameComms only in a helper |
| X-Men Legends, X-Men Legends II | own sockets and SDP records |
| Worms World Party | the Mophun runtime (MOPHUN.DLL) has its own sockets and SDP records |

Possible route: the bridge acts as the other phone (inquiry name of the
remote host, RFCOMM server or client on the game's channel) and tunnels
the raw bytes. One bridge address can stand in for one remote phone per
channel.

### 2. The game tags its Bluetooth name

Many 2.x games write a game tag, their role and a session id into the
phone's Bluetooth name and only accept hosts or clients that carry it.
Rifts (source available) uses `6R49H<8 hex digits><name>` for a host and
`6R49C...` for a client and sets the name itself with
`RHostResolver::SetHostName` right before StartHostL / StartClientL. SSX
builds a 3 character tag the same way. Others go through
SetLocalDeviceName (Catan, Mile High Pinball, The Roots, Warhammer,
Snakes, Glimmerati).

The replacement used to take the name once, when the GameComms object
was created, and sent it with the registration and the host open. A
renamed host was listed without its tag, so clients rejected it, and a
renamed client reached the host under its old name.

Now (`FollowBtName=1`, default) the engine re-reads the Bluetooth name
at StartHostL, StartClientL, SetHostAcceptMode, DeclareRemoteDevice and
Connect. A changed name, or one set with SetLocalDeviceName, is used for
the next registration and host open; an idle client registers again and
a host whose session nobody has joined re-opens it under the new name.
A name that was never changed keeps DeviceName= from GameComms.ini.
Names are still limited to 32 bytes.

### 3. "Bluetooth busy" while the bridge is connected

Every failing game that uses GameComms (and most of the others) asks
the Bluetooth stack for the number of ACL links (`KLMGetACLLinkCount` on
an L2CAP socket) before it starts a host or a client, and refuses with
"Bluetooth busy" if there is any. Glimmerati, for example, does this
after every reset of its network code. With the original library there
was no link between games; the replacement kept the bridge connection
for as long as the GameComms object lived, so the second attempt in a
session always failed.

Now (`ReleaseBridge=1`, default) the bridge connection is closed about
1.5 seconds after Disconnect() or EndSession() when nothing needs it any
more (no search, no join, no connection; a host neither accepts players
nor has any). Every call that needs the network again reconnects.

### 4. Smaller problems found on the way

- Ordinals 59 and 60 (v10, v11) are getters; the replacement left the
  TBool& untouched (now: not limited, visible).
- The helper classes found the GameComms engine through a single
  thread-local pointer. Deleting a second, short-lived GameComms object
  (Pathway to Glory creates them for the power state) cleared it, and
  the discoverer then reported nothing. Engines are now kept in a list.
- The discoverer gave up after 10 seconds even while the bridge
  selection dialog was open. Time spent in that dialog no longer counts.

### Not resolved

- Catan uses the exported CGameBTDeviceList only as a container (it never
  calls Show), which the inert list supports.
- The King of Fighters Extreme and One do not rename the phone. Apart
  from the "Bluetooth busy" check no cause was found statically; a
  GameComms.log from hardware is needed.
- Pandemonium! is packed (the .APP files are small loaders) and cannot
  be analysed statically.
- Snakes uses GameComms and its own SDP and RFCOMM code side by side.

## Limits common to all variants

- At most 1024 bytes per send. The 2.x originals used 512 byte buffers,
  1.x documents no limit. Larger sends return KErrOverflow. A send also
  returns KErrOverflow while 16 messages are still unacknowledged on the
  Bluetooth connection (the original would block instead).
- Latency is higher than on a direct Bluetooth connection: expect 20 to
  60 ms round trip on a local network, plus the network delay over the
  Internet.
- All callbacks are asynchronous, never from inside an API call.
- IsShowingDeviceSelectDlg also returns true while the one-time bridge
  selection dialog is shown.
