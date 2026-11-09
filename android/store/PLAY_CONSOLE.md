# Releasing on Google Play

Everything the Play Console asks for, in the order it asks. The texts and
graphics are in `android/fastlane/metadata/android/` (the standard fastlane
layout, so `fastlane supply` can upload them; copying them in by hand works
just as well).

## 0. Before you start

- Google Play developer account (one-time fee). New personal accounts must
  run a **closed test with at least 12 testers for 14 days** before they can
  publish to production; check the current rule in the Play Console.
- Replace `CONTACT_EMAIL` in `android/PRIVACY.md`, commit it, and use its
  GitHub URL as the privacy policy URL:
  `https://github.com/ngagesdk/PhantomLink/blob/main/android/PRIVACY.md`
- Build a signed bundle: see `android/README.md`, "Release build".

## 1. Create the app

| Field | Value |
|---|---|
| App name | PhantomLink Bridge |
| Default language | English (United States), en-US |
| App or game | App |
| Free or paid | Free |

## 2. Store listing (Grow > Store presence > Main store listing)

| Field | File |
|---|---|
| App name (30) | `en-US/title.txt` |
| Short description (80) | `en-US/short_description.txt` |
| Full description (4000) | `en-US/full_description.txt` |
| App icon 512x512 | `en-US/images/icon.png` |
| Feature graphic 1024x500 | `en-US/images/featureGraphic.png` |
| Phone screenshots (2 to 8) | `en-US/images/phoneScreenshots/1_*.png` to `5_*.png` (1080x2160) |

Add a German translation (Translations > Add > German, de-DE) with the
files in `de-DE/`; the graphics are shared.

The screenshots are renders of the app's screens (same layout, colours and
texts as the Compose code). Before publishing, compare them with the app on
a phone and replace any that differ with real captures
(`adb exec-out screencap -p > shot.png`, 1080 px wide, 2:1 at most).

Store settings:

| Field | Value |
|---|---|
| Category | Tools |
| Tags | Gaming accessories, Connectivity (or the closest offered) |
| Email | your support address |
| Website | https://github.com/ngagesdk/PhantomLink |

## 3. App content (Policy > App content)

**Privacy policy**: the URL from step 0.

**Ads**: No, the app does not contain ads.

**App access**: All functionality is available without special access (no
login). Note for reviewers, in "Any other instructions":

> PhantomLink Bridge forwards Bluetooth multiplayer traffic from a Nokia
> N-Gage (2003 handheld) to a PhantomLink server. Without an N-Gage the app
> starts, shows its status, finds servers and logs events, but no game
> traffic flows. Source: https://github.com/ngagesdk/PhantomLink

**Content rating** (IARC questionnaire):
- Category: "All other app types" (utility).
- Violence, sexuality, language, controlled substances, gambling: No.
- Users can interact or exchange content: **Yes** (players of the forwarded
  N-Gage games play together through the server). Shares location: No.
  Allows purchases: No.

**Target audience**: 13 and older (13-15, 16-17, 18+). Not designed for
children. Appeals to children: No.

**News app**: No. **COVID-19 app**: No. **Government app**: No.
**Financial features**: None. **Health**: None.

**Data safety** (my reading of the form; the final answers are yours):
- Collects or shares data: **Yes**. The app sends the N-Gage's Bluetooth
  address and player name to the server the user picks, which Google counts
  as collection even though the developer never receives it.
- Encrypted in transit: **No** (the N-Gage protocol cannot use TLS).
- Users can request deletion: No account exists; the developer stores
  nothing. Answer "No" and explain in the privacy policy (already done).
- Data types:
  - Device or other IDs: collected, not shared, required, purpose *App
    functionality*. (N-Gage Bluetooth address.)
  - Personal info > Name: collected, not shared, required, purpose *App
    functionality*. (Player nickname from the N-Gage.)
  - Nothing else (no location, contacts, files, app activity, diagnostics).

**Foreground service permissions**: the app declares
`FOREGROUND_SERVICE_CONNECTED_DEVICE`. Select *Connected device* and enter:

> The bridge keeps a Bluetooth serial connection to a Nokia N-Gage open and
> forwards its multiplayer traffic to a game server while a game is in
> progress. The game runs on the N-Gage, so the phone's screen is usually
> off. The service starts only when the user taps Start, shows an ongoing
> notification with a Stop action, and stops when the user stops it.
> Interrupting it would end the multiplayer game for all players.

Google asks for a video link showing the feature: record the phone (Start,
N-Gage connects, notification visible, screen off, game keeps running) and
upload it as an unlisted YouTube video.

**Advertising ID**: No (the app does not use it).

## 4. Release

1. Testing > Closed testing > Create track, add testers (an email list or a
   Google Group).
2. Create release, upload `app-release.aab`, release name `1.0.0 (1)`,
   release notes from `en-US/changelogs/1.txt` (and `de-DE`).
3. Countries: all, or where your players are.
4. Review and roll out. After the testing period, promote the release to
   Production.

Play App Signing is on by default for new apps: Google keeps the app
signing key, you keep the upload key (your keystore). Back the keystore up;
losing it means asking Google for an upload key reset.

## 5. Each update

1. Raise `versionCode` (and `versionName`) in `android/app/build.gradle.kts`.
2. Add `fastlane/metadata/android/<lang>/changelogs/<versionCode>.txt`.
3. `./gradlew bundleRelease`, upload, roll out.
