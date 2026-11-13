# PhantomLink Bridge for Android: Privacy Policy

Last updated: 8 November 2026

PhantomLink Bridge ("the app") is an open source app that forwards the
multiplayer traffic of N-Gage games between an N-Gage connected over
Bluetooth and a PhantomLink server. This policy explains what data the app
handles.

## Summary

- The app has no account, no advertising, no analytics and no tracking.
- The developer does not receive any data from the app.
- Game data goes only to the PhantomLink server that you choose (one found
  on your local network, or an address you enter).

## Data the app sends to the server you choose

To let you play with others, the app forwards what the N-Gage sends and
adds what the server needs to tell players apart:

| Data | Why |
|---|---|
| Bluetooth address of the connected N-Gage | identifies your N-Gage across short connection drops |
| Player name set on the N-Gage | shown to the other players |
| Game and session data from the N-Gage | the multiplayer game itself |
| Access key, if you entered one | lets the server admit you |
| Your Internet (IP) address | needed for any Internet connection |

This traffic is not encrypted, because the N-Gage cannot use encryption.
Use a server you trust. The server is run by whoever operates it, not by the
app's developer; their own privacy terms apply to it.

## Data stored on your phone

- Settings (server address, port, access key, PIN, service name, theme).
- A log of bridge events, kept in memory while the app runs. It is only
  shared if you tap "Share log" yourself.

Uninstalling the app deletes all of it.

## Permissions

- **Nearby devices (Bluetooth)**: accept the N-Gage's connection, answer
  its pairing PIN and, when you tap "Make visible", let the N-Gage find the
  phone.
- **Internet and network state**: connect to the server and find servers on
  the local network.
- **Notifications and foreground service**: keep the bridge running and
  show that it runs while the screen is off.

## Children

The app is not directed at children under 13.

## Changes and contact

Changes to this policy are published in the project repository:
https://github.com/ngagesdk/PhantomLink/blob/main/android/PRIVACY.md

Questions: open an issue at https://github.com/ngagesdk/PhantomLink/issues
or write to mail@michael-fitzmayer.de.
