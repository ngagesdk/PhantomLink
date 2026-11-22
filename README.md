# PhantomLink

[![PhantomLink](https://raw.githubusercontent.com/ngagesdk/PhantomLink/main/media/logo-modern.png)](https://raw.githubusercontent.com/ngagesdk/PhantomLink/main/media/logo-modern.png?raw=true "PhantomLink")

[![Continuous Integration](https://github.com/ngagesdk/PhantomLink/actions/workflows/continuous-integration.yml/badge.svg)](https://github.com/ngagesdk/PhantomLink/actions/workflows/continuous-integration.yml)
[![Join Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?logo=discord&logoColor=white)](https://discord.gg/z3yzYPC49J)

**Play Nokia N-Gage multiplayer games online again.**

Many N-Gage games could only be played together over Bluetooth, with all
players in the same room. PhantomLink takes that local multiplayer to your
network and the Internet, without changing the games themselves.

It works like this: a small helper, the **bridge**, connects to your N-Gage
over Bluetooth and passes the game's data to a **PhantomLink server**, which
connects you with other players of the same game. The bridge can be an
Android phone, a Linux PC or Raspberry Pi, a Windows PC or an ESP32 board.

PhantomLink also brings back **N-Gage Arena**, Nokia's online service for
the N-Gage, which was shut down in 2010, and the online features of other
games that had their own servers.

## How it works

The N-Gage app installs a drop-in replacement for `GameComms.dll`. Games that
use it talk to a dedicated bridge service, allowing multiplayer games originally
limited to local play to connect online. The app also emulates the Nokia N-Gage
Arena service: games can access it directly over GPRS or through a Bluetooth
bridge.

The bridge service connects to the PhantomLink server, where the individual
games' server logic is handled.

## Features

- Online multiplayer for N-Gage games that originally used Bluetooth.
- Works with original retail game cards.
- Up to four players per game, just like the original.
- An N-Gage app that installs everything a game needs with a few clicks.
- N-Gage Arena: accounts, chat login and game services for selected games.
- Other online services, such as the Habbo Console of Habbo Islands.

## Quickstart

1. **Set up the Android bridge.** Install the app and choose a server in
   Settings. To use the prefilled server, turn off **Find server automatically**.
   For your own server, use automatic discovery on the local network or enter
   its address and access key. All players must use the same server.

   [![Get it on Google Play](https://play.google.com/intl/en_us/badges/images/generic/en_badge_web_generic.png)](https://play.google.com/store/apps/details?id=de.ngagesdk.phantomlink)

2. **Pair the devices.** Start the bridge, allow the requested permissions and
   tap **Make visible**. On the N-Gage, open Bluetooth, select **Paired devices**
   and pair with the Android phone. Enter the PIN from the app settings when
   pairing (default `1234`).
3. **Set up the N-Gage.** Open **Settings**, then **Connection**, then **Access
   points**. Create an access point named `PhantomLink`, select **GSM data** and
   enter any number. In the Android app, tap **Send PhantomLink.sis to phone**,
   select the N-Gage and accept the file. Install and open it, then pick your
   game. The Symbian app handles the remaining setup.
4. **Play.** Start a multiplayer game and select the paired bridge or the
   PhantomLink access point (when connecting to the N-Gage Arena).

For other bridges, running your own server or alternative installation steps,
see [docs/BUILD.md](docs/BUILD.md). Downloads are available on the
[Releases page](https://github.com/ngagesdk/PhantomLink/releases/tag/continuous-integration).

For N-Gage Arena, choose **Arena over BT** in the PhantomLink app; see
[docs/ARENA.md](docs/ARENA.md).

## Compatibility

### Bridges

| Bridge                     | Status                                                    |
| -------------------------- | --------------------------------------------------------- |
| Android phone (Android 8+) | Supported                                                 |
| Linux PC / Raspberry Pi    | Supported                                                 |
| ESP32 board                | Supported (one ESP32 per N-Gage)                          |
| Windows PC                 | Limited: may not work, depending on the Bluetooth adapter |

### Multiplayer games

| Game                                     |                              Status                              |
| ---------------------------------------- | :--------------------------------------------------------------: |
| Ashen                                    |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Asphalt Urban GT                         |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Asphalt: Urban GT 2                      |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Bomberman                                |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Call of Duty                             |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Catan                                    | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Crash Nitro Kart                         | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| FIFA Football 2004                       |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| FIFA Football 2005                       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Glimmerati                               | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Marcel Desailly Pro Soccer               |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Mile High Pinball                        |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| NCAA Football 2004                       |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| ONE                                      | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Operation Shadow                         |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Puyo Pop                                 |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Puzzle Bobble VS                         | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Rayman 3                                 |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Requiem of Hell                          | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Rifts: Promise of Power                  |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| SSX Out of Bounds                        | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| The King of Fighters Extreme             | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| The Roots: Gates of Chaos                |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Tiger Woods PGA Tour 2004                |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Tom Clancy's Ghost Recon: Jungle Storm   |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Tom Clancy's Splinter Cell: Chaos Theory | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Tony Hawk's Pro Skater                   |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Warhammer 40,000: Glory in Death         |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| WWE Aftershock                           |   ![Working](https://img.shields.io/badge/Working-brightgreen)   |
| Xanadu Next                              | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |

Games that are not listed use their own Bluetooth code instead of the
standard GameComms library, so PhantomLink cannot support them.

### N-Gage Arena games

| Game                            | Online feature                                                  |                            Status                            |
| ------------------------------- | --------------------------------------------------------------- | :----------------------------------------------------------: |
| Pocket Kingdom: Own the World   | Battles against other players' kingdoms, auctions, rankings     | ![Working](https://img.shields.io/badge/Working-brightgreen) |
| Rifts: Promise of Power         | Challenges and turn-based battles "by e-mail"                   | ![Working](https://img.shields.io/badge/Working-brightgreen) |
| Ashen                           | Online leaderboards for every chapter and the whole game        |   ![Untested](https://img.shields.io/badge/Untested-grey)    |
| Worms World Party               | Play Now game rooms (Freestyle, Ranked), skill points, rankings |   ![Untested](https://img.shields.io/badge/Untested-grey)    |
| Pathway to Glory                | GPRS battles for up to 6 players, Field Radio voice messages    | ![Working](https://img.shields.io/badge/Working-brightgreen) |
| Pathway to Glory: Ikusa Islands | GPRS battles for up to 6 players, Field Radio voice messages    | ![Working](https://img.shields.io/badge/Working-brightgreen) |
| SSX Out of Bounds               | Shadow racing (upload, download, top times), careers            |   ![Untested](https://img.shields.io/badge/Untested-grey)    |

Pocket Kingdom and Rifts are tested on a real N-Gage.
Ashen's leaderboards are tested against the server only, not yet on a
real N-Gage. The same goes for Worms World Party, whose server side is
written from the game's Arena libraries (see
[docs/ARENA.md](docs/ARENA.md#worms-world-party)).
Both Pathway to Glory games have a game server of their own, written
from the game's network code (see
[docs/ARENA.md](docs/ARENA.md#pathway-to-glory-ikusa-islands)); it is
tested against simulated games only.
SSX Out of Bounds belongs to the first Arena generation and has its own
encrypted protocol; its server side is written from the game and tested
against a simulated game only (see [docs/SSX.md](docs/SSX.md)).

### Other online games

These games had their own online service instead of N-Gage Arena.
PhantomLink's server provides it with the same accounts as the Arena
games, and the same **Arena over BT** / **Arena over GPRS** setup in the
PhantomLink app points the game to it.

| Game          | Online feature                                    |                            Status                            |
| ------------- | ------------------------------------------------- | :----------------------------------------------------------: |
| Habbo Islands | Habbo Console: friends, messages, friend requests | ![Working](https://img.shields.io/badge/Working-brightgreen) |

Habbo Islands is tested against the server only, not yet on a real N-Gage;
see [docs/HABBO.md](docs/HABBO.md).

## Documentation

- [docs/BUILD.md](docs/BUILD.md) - building, installing and configuring every part
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) - games, DLL variants, checksums, known problems
- [docs/ARENA.md](docs/ARENA.md) - N-Gage Arena setup, accounts and protocols
- [docs/HABBO.md](docs/HABBO.md) - Habbo Islands' Habbo Console
- [docs/SSX.md](docs/SSX.md) - SSX Out of Bounds online service
- [docs/DESIGN.md](docs/DESIGN.md) - architecture and implementation status
- [docs/PROTOCOL.md](docs/PROTOCOL.md) - wire protocol
- [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) - analysis of the original DLLs
- [docs/SECURITY_AUDIT.md](docs/SECURITY_AUDIT.md) - security audit of the server
- [docs/HISTORY.md](docs/HISTORY.md) - project history and the name
- [android/README.md](android/README.md) - Android bridge app

## License

Copyright 2023-2026 Michael Fitzmayer. Released under the MIT license, see
[LICENSE.md](LICENSE.md).

The files in `dll/symbian/bt/` are based on Nokia's Bluetooth example code.

Both [PhantomLink logos](media/) were provided by [Dan Whelan](https://danwhelan.ie/)
and are released under the
[CC0 1.0 Universal license](https://creativecommons.org/publicdomain/zero/1.0/).

[![Co-created with AI](https://madebyhuman.iamjarl.com/badges/co-created-white.svg)](https://madebyhuman.iamjarl.com)
