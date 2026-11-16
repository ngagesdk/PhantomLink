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

## Features

- Online multiplayer for N-Gage games that originally used Bluetooth.
- Works with original retail game cards.
- Up to four players per game, just like the original.
- An N-Gage app that installs everything a game needs with a few clicks.
- N-Gage Arena: accounts, chat login and game services for selected games.
- Other online services, such as the Habbo Console of Habbo Islands.

## Compatibility

### Multiplayer games

| Game                                     |                              Status                              |
| ---------------------------------------- | :--------------------------------------------------------------: |
| Ashen                                    | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Asphalt Urban GT                         | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Asphalt: Urban GT 2                      | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Bomberman                                | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Call of Duty                             | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Catan                                    | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Crash Nitro Kart                         | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| FIFA Football 2004                       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| FIFA Football 2005                       | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Glimmerati                               | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Marcel Desailly Pro Soccer               | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Mile High Pinball                        | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| NCAA Football 2004                       | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| ONE                                      | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Operation Shadow                         | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Puyo Pop                                 | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Puzzle Bobble VS                         | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Rayman 3                                 | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Requiem of Hell                          | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Rifts: Promise of Power                  | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| SSX Out of Bounds                        | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| The King of Fighters Extreme             | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| The Roots: Gates of Chaos                | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Tiger Woods PGA Tour 2004                | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Tom Clancy's Ghost Recon: Jungle Storm   | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Tom Clancy's Splinter Cell: Chaos Theory | ![Not%20Working](https://img.shields.io/badge/Not%20Working-red) |
| Tony Hawk's Pro Skater                   | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Warhammer 40,000: Glory in Death         | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| WWE Aftershock                           | ![Working](https://img.shields.io/badge/Working-brightgreen)     |
| Xanadu Next                              | ![Untested](https://img.shields.io/badge/Untested-grey)          |

Games that are not listed use their own Bluetooth code instead of the
standard GameComms library, so PhantomLink cannot support them.

### N-Gage Arena games

| Game                          | Online feature                                                  | Status                                                              |
| ----------------------------- | --------------------------------------------------------------- | :-----------------------------------------------------------------: |
| Pocket Kingdom: Own the World | Battles against other players' kingdoms, auctions, rankings     | ![Working](https://img.shields.io/badge/Working-brightgreen)        |
| Rifts: Promise of Power       | Challenges and turn-based battles "by e-mail"                   | ![Working](https://img.shields.io/badge/Working-brightgreen)        |
| Ashen                         | Online leaderboards for every chapter and the whole game        | ![Untested](https://img.shields.io/badge/Untested-grey)             |
| Worms World Party             | Play Now game rooms (Freestyle, Ranked), skill points, rankings | ![Untested](https://img.shields.io/badge/Untested-grey)             |

Pocket Kingdom logs in and reaches its Arena menus on a real N-Gage; the
game server for the online map, battles, chat and the Global Auction is
tested against simulated games only.
Ashen's leaderboards are tested against the server only, not yet on a
real N-Gage. The same goes for Worms World Party, whose server side is
written from the game's Arena libraries (see
[docs/ARENA.md](docs/ARENA.md#worms-world-party)).

### Other online games

These games had their own online service instead of N-Gage Arena.
PhantomLink's server provides it with the same accounts as the Arena
games, and the same **Arena over BT** / **Arena over GPRS** setup in the
PhantomLink app points the game to it.

| Game          | Online feature                                    | Status                                                       |
| ------------- | ------------------------------------------------- | :----------------------------------------------------------: |
| Habbo Islands | Habbo Console: friends, messages, friend requests | ![Working](https://img.shields.io/badge/Working-brightgreen) |

Habbo Islands is tested against the server only, not yet on a real N-Gage;
see [docs/HABBO.md](docs/HABBO.md).

### Bridges

| Bridge                     | Status                                                               |
| -------------------------- | -------------------------------------------------------------------- |
| Android phone (Android 8+) | Tested with simulated N-Gages                                        |
| Linux PC / Raspberry Pi    | Tested with simulated N-Gages                                        |
| ESP32 board                | Supported (one ESP32 per N-Gage)                                     |
| Windows PC                 | Limited: may not work, depending on the Bluetooth adapter            |

## What you need

- A Nokia N-Gage (Classic or QD) and a supported game.
- A bridge: one of the devices listed above.
- A PhantomLink server that all players connect to.
- An Internet or local network connection for the bridge.

## Getting started

Download `PhantomLink.zip` from the
[Releases page](https://github.com/ngagesdk/PhantomLink/releases/tag/continuous).
It contains the server (Linux, Windows), the Android and Windows bridges,
the ESP32 firmware and the N-Gage app `PhantomLink.sis`.

1. **Start a server.** Run `gamecommsd` on a computer in your network.
   To play over the Internet, all players connect to the same server
   through a VPN.
2. **Set up a bridge.** The easiest choice is the Android app:

   [![Get it on Google Play](https://play.google.com/intl/en_us/badges/images/generic/en_badge_web_generic.png)](https://play.google.com/store/apps/details?id=de.ngagesdk.phantomlink)

   Open it and tap start. For the other bridges, see
   [docs/BUILD.md](docs/BUILD.md).
3. **Prepare the N-Gage.** Install `PhantomLink.sis`, open PhantomLink and
   pick your game from the list. (If the game has its own `gamecomms.dll`
   in its folder, see [docs/BUILD.md](docs/BUILD.md), "Installing on the
   N-Gage".)
4. **Play.** Start a multiplayer game and select the bridge when asked
   (PIN `1234`).

For N-Gage Arena, choose **Arena over BT** in the PhantomLink app; see
[docs/ARENA.md](docs/ARENA.md).

## Documentation

- [docs/BUILD.md](docs/BUILD.md) - building, installing and configuring every part
- [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) - games, DLL variants, checksums, known problems
- [docs/ARENA.md](docs/ARENA.md) - N-Gage Arena setup, accounts and protocols
- [docs/HABBO.md](docs/HABBO.md) - Habbo Islands' Habbo Console
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
