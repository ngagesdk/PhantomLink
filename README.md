# PhantomLink

[![PhantomLink](https://raw.githubusercontent.com/ngagesdk/PhantomLink/main/media/logo-modern.png)](https://raw.githubusercontent.com/ngagesdk/PhantomLink/main/media/logo-modern.png?raw=true "PhantomLink")

[![Continuous Integration](https://github.com/ngagesdk/PhantomLink/actions/workflows/continuous-integration.yml/badge.svg)](https://github.com/ngagesdk/PhantomLink/actions/workflows/continuous-integration.yml)
[![Join Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?logo=discord&logoColor=white)](https://discord.gg/dbUzqJ26vs)

PhantomLink aims to provide drop-in replacements for **GameComms**, the proprietary
multiplayer communication library included in the Nokia N-Gage SDK.

GameComms provided a unified API that allowed up to four Nokia N-Gage devices to
communicate with each other over Bluetooth. PhantomLink replaces this functionality,
enabling existing N-Gage multiplayer games to be played over Wi-Fi and the Internet
without requiring any modifications to the original games.

To achieve this, PhantomLink uses a bridge device that communicates with the N-Gage
over Bluetooth and relays multiplayer data to a central server. The bridge can run
on an ESP32 development board or on a computer with Bluetooth support, such as a
Linux system (including a Raspberry Pi) or, with some limitations, Windows.

The server handles matchmaking between players running the same game and forwards
communication data between their devices, effectively extending the original
Bluetooth multiplayer functionality across the Internet.

PhantomLink also brings back **N-Gage Arena**, Nokia's online service for the
N-Gage, for games that used it (see [N-Gage Arena](#n-gage-arena) below).

## Project History

PhantomLink was originally started in **2023**. However, development was put on hold
due to the lack of useful documentation on GameComms and the challenges involved in
understanding and reproducing its underlying behavior.

In **2026**, the project was handed over to **Claude** with the goal of finally
bringing it into a functional, working state and turning the original concept into a
usable replacement for GameComms.

The ultimate goal remains simple: bring online multiplayer back to the Nokia N-Gage,
allowing players to enjoy existing multiplayer games regardless of physical distance.

## Why PhantomLink?

The name **PhantomLink** captures the idea of playing local multiplayer without being
physically local. The _Phantom_ represents the invisible presence of players who are
connected from somewhere else, seemingly sharing the same room despite being miles
apart. The _Link_ is the connection that makes this possible, bridging the gap between
the original Bluetooth experience and the world beyond. Together, PhantomLink lets
distant players become phantoms in each other's local multiplayer sessions.

## Requirements

To use PhantomLink and play Nokia N-Gage multiplayer games online, you need the
following:

### 1. A Nokia N-Gage game

- PhantomLink replaces the original `GAMECOMMS.DLL` to enable its functionality.

- PhantomLink works with any game supported by the project, including original
  retail N-Gage cartridges.

- N-Gage Arena games keep all their original files; they go online through
  the PhantomLink app's Arena setting (see [N-Gage Arena](#n-gage-arena)).

### 2. A Bluetooth bridge device

You need one of the following devices to connect your N-Gage to PhantomLink:

- An ESP32 development board with Bluetooth support.

- An Android phone (Android 8 or newer).

[![Get it on Google Play](https://play.google.com/intl/en_us/badges/images/generic/en_badge_web_generic.png)](https://play.google.com/store/apps/details?id=de.ngagesdk.phantomlink)

- A Linux PC with Bluetooth support.

- A Raspberry Pi with Bluetooth support.

- A Windows PC with Bluetooth support (some limitations apply).

### 3. An Internet connection

- The bridge device needs Internet access to communicate with the PhantomLink server.
- All players must connect to the same server to find each other and play together.

### 4. A Nokia N-Gage device

- Obviously, you need a working Nokia N-Gage (Classic or QD) capable of running the
  game you want to play.

That's it! PhantomLink takes care of bridging the original Bluetooth multiplayer
communication over the Internet, allowing you to play classic N-Gage multiplayer
games with other players regardless of physical distance.

## Contents

- `dll/` - replacement GAMECOMMS.DLL for all 11 original variants
- `firmware/` - ESP32 bridge firmware
- `pcbridge/` - PC bridge for Linux and Windows (`gcbridge`), same job as the ESP32
- `android/` - Android bridge app, same job as the ESP32, same bridge engine
- `server/` - endpoint server (`gamecommsd`), including the N-Gage Arena
  back ends
- `protocol/` - protocol library shared by all three parts
- `tests/` - automated tests
- `tools/` - variant identification and reverse engineering scripts;
  `tools/gc_usage.py` shows how a game uses GameComms

## Documentation

- [docs/BUILD.md](docs/BUILD.md) - build, flash, deploy, configure, test
- [docs/DESIGN.md](docs/DESIGN.md) - architecture
- [docs/PROTOCOL.md](docs/PROTOCOL.md) - wire protocol
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) - variants, games, exports
- [docs/ARENA.md](docs/ARENA.md) - N-Gage Arena: setup, accounts, protocols, game servers
- [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) - analysis of the original DLLs
- [docs/SECURITY_AUDIT.md](docs/SECURITY_AUDIT.md) - Security audit: gamecommsd

## Quick start

1. Server: build and run `gamecommsd` on a PC in your network
   (see docs/BUILD.md).
2. Bridge, one of:
   - Linux computer with Bluetooth, for example a Raspberry Pi: build and
     start `gcbridge`. One computer can serve several N-Gages.
   - Windows PC: `gcbridge.exe` works the same way, but depends on the
     Bluetooth adapter and may not work (see docs/BUILD.md).
   - Android phone: install the app from `android/` (see
     [android/README.md](android/README.md)), tap start. One phone can
     serve several N-Gages.
   - ESP32: flash it with `pio run -d firmware -t upload`, connect to its
     `GameCommsBridge-Setup` Wi-Fi and enter your Wi-Fi settings. One
     ESP32 per N-Gage.
3. N-Gage: replace the game's `gamecomms.dll` with the matching build
   (see the list below), start a multiplayer game and select the bridge
   when asked. Or install `PhantomLink.sis`: it copies the right
   `gamecomms.dll` to `C:\System\Libs` for the game you pick and can
   configure the ESP32 bridge over Bluetooth (docs/BUILD.md). Choose
   **Restore original behavior** in the game list to remove the default DLL.
   In the game list, Up and Down wrap at the ends; Left and Right move
   six entries at a time, stopping at the first or last entry.

## Which DLL for which game

Replace the game's `gamecomms.dll` with the build from the "Use" column
(`build/dll/out/vN/gamecomms.dll`). You can check the original with its
md5sum, or with `python tools/gc_identify.py dll gamecomms.dll`.

| Game                                     | Use      | md5sum of the original           | Ordinals |                              Status                              |
| ---------------------------------------- | -------- | -------------------------------- | -------- | :--------------------------------------------------------------: |
| Marcel Desailly Pro Soccer               | v1       | 79c25cdd1972c287712475ee544c7bf3 | 33       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Puyo Pop                                 | v2       | 8863fdea897f1c63807ad69252a3566f | 55       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Puzzle Bobble VS                         | v2       | 8863fdea897f1c63807ad69252a3566f | 55       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Tony Hawk's Pro Skater                   | v2       | 8863fdea897f1c63807ad69252a3566f | 55       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Asphalt Urban GT                         | v3       | f6fbc3c745ae0ecf584b3ef7ec4371ef | 56       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| FIFA Football 2004                       | v3       | f6fbc3c745ae0ecf584b3ef7ec4371ef | 56       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| NCAA Football 2004                       | v3       | f6fbc3c745ae0ecf584b3ef7ec4371ef | 56       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Rayman 3                                 | v3       | f6fbc3c745ae0ecf584b3ef7ec4371ef | 56       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Tom Clancy's Ghost Recon: Jungle Storm   | v3       | f6fbc3c745ae0ecf584b3ef7ec4371ef | 56       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Catan                                    | v5       | e8d317204a763d71215680d567eb71e0 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Mile High Pinball                        | v5       | e8d317204a763d71215680d567eb71e0 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Requiem of Hell                          | v5       | e8d317204a763d71215680d567eb71e0 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| The King of Fighters Extreme             | v5       | e8d317204a763d71215680d567eb71e0 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Tom Clancy's Splinter Cell: Chaos Theory | v5       | e8d317204a763d71215680d567eb71e0 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Call of Duty                             | v6       | bb02377c5361597f8d8503f5de584845 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| FIFA Football 2005                       | v6       | bb02377c5361597f8d8503f5de584845 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| SSX Out of Bounds                        | v6       | bb02377c5361597f8d8503f5de584845 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| WWE Aftershock                           | v6       | bb02377c5361597f8d8503f5de584845 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Xanadu Next                              | v6       | bb02377c5361597f8d8503f5de584845 | 57       | ![Untested](https://img.shields.io/badge/Untested-grey)          |
| Ashen                                    | v8       | d4c10fcebfb6da8da6b757aa8392b6d7 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Bomberman                                | v8       | d4c10fcebfb6da8da6b757aa8392b6d7 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Crash Nitro Kart                         | v8       | d4c10fcebfb6da8da6b757aa8392b6d7 | 57       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Operation Shadow                         | v8       | d4c10fcebfb6da8da6b757aa8392b6d7 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |                                                                 |
| Tiger Woods PGA Tour 2004                | v9       | 6ac8b1a1e490b367a1d5e46bb720fe70 | 57       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Glimmerati                               | v10      | 827df54717bc210daa96aef9e12de4a3 | 63       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| ONE                                      | v10      | 827df54717bc210daa96aef9e12de4a3 | 63       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Rifts: Promise of Power                  | v10      | 827df54717bc210daa96aef9e12de4a3 | 63       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| The Roots: Gates of Chaos                | v10      | 827df54717bc210daa96aef9e12de4a3 | 63       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Asphalt: Urban GT 2                      | v11      | 75536306ef48b1928b4562890d01c9c6 | 63       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Warhammer 40,000: Glory in Death         | v11      | 75536306ef48b1928b4562890d01c9c6 | 63       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |

Always use the matching version. v1 and v2 to v11 have different APIs and
cannot be mixed. Other combinations are not tested. Games not listed are
unsupported because they use their own custom Bluetooth stack or
implementation.

## N-Gage Arena

N-Gage Arena was Nokia's online service for the N-Gage: a community web
service with user accounts and profiles, chat and friends lists (SegaChat),
and the SNAP lobby and game room service on which games ran their online
modes. Arena games bring their own network stack on the cartridge instead of
GameComms, and the service was shut down in 2010.

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
   (SOAP), SegaChat (Jabber), SNAP (lobbies, game rooms) and the game servers
   of the individual games.

One Arena account works for all games. The first login with a new user name
creates the account and binds the user name, the password and the N-Gage
(its IMEI) together; later logins need the same password on the same N-Gage,
and one N-Gage can have one account. The server keeps the accounts in
`gamecomms-arena.txt` (passwords only as SHA-1). See
[docs/ARENA.md](docs/ARENA.md) for setup and details.

### Supported Arena games

| Game                          | Arena features                                                                     | Server side                                                        |                                Status                                |
| ----------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------ | :------------------------------------------------------------------: |
| Pocket Kingdom: Own the World | Battles against other players' kingdoms, auctions, rankings                        | Login, account and profile pages, SegaChat login; game server open | ![In%20progress](https://img.shields.io/badge/In%20progress-yellow) |
| Rifts: Promise of Power       | Challenges and turn-based battles "by e-mail" (melee, survivor, defender, KotH, CTF) | Login, SNAP lobby and room, Rifts game server (battles, turns)     | ![Untested](https://img.shields.io/badge/Untested-grey)             |

Pocket Kingdom logs in and reaches its Arena menus on a real N-Gage. The Rifts
game server istested against simulated games, but has not run with a real N-Gage
yet. Rifts' Bluetooth multiplayer works with the GameComms replacement (see the
table above), independently of Arena.

## Status

- Protocol, server, bridge and DLL logic: implemented and covered by
  automated tests.
- ESP32 firmware: implemented, built in CI.
- PC bridge: implemented and tested with simulated devices (Linux: against
  a simulated BlueZ). Linux still has to be tested with a real N-Gage. On
  Windows the first real test failed inside the Windows Bluetooth stack
  (see docs/BUILD.md, "Windows: known problem").
- Android bridge: implemented; its engine is tested on a PC against the real
  server and simulated N-Gages. Still has to be tested with a real N-Gage.
- Symbian DLLs: implemented. They need the Windows N-Gage toolchain to
  build and still have to be tested on real hardware.
- N-Gage Arena: PPP route, Community service, SegaChat login and shared
  accounts work with Pocket Kingdom on a real N-Gage. SNAP and the Rifts game
  server are tested against simulated games only.

## License

MIT, see [LICENSE.md](LICENSE.md).  

[![Co-created with AI](https://madebyhuman.iamjarl.com/badges/co-created-white.svg)](https://madebyhuman.iamjarl.com)

The files in `dll/symbian/bt/` are based on Nokia's Bluetooth example code.

Both [PhantomLink logos](media/) were provided by [Dan Whelan](https://danwhelan.ie/)
and are released under the
[CC0 1.0 Universal license](https://creativecommons.org/publicdomain/zero/1.0/).

You are free to use, modify, and distribute them without restriction.
