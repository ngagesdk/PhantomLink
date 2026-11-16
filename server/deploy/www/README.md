# Arena device pages

Example pages for the server's `www_root` setting (docs/ARENA.md, "Device
pages"). Copy this folder next to the server, point `www_root` at it and
edit the pages; changes show up on the next request, no restart needed.

A request for `/n-gage/...` is answered with:

1. `www_root/<game class>/n-gage/...` when it exists, for the game the
   user is logged in with (Rifts: Promise of Power is 8053),
2. else `www_root/n-gage/...`,
3. else the page the server generates: rankings, tournaments, activities
   and games of the Play menu (docs/ARENA.md, "Rankings, tournaments and
   activities"),
4. else the server's built-in placeholder (an empty list for other
   `*_x.jsp`, a short black page otherwise).

A file here hides the generated page with the same path. The example
`n-gage/device/global/play/activities/news.jsp` does that for the
activity news: delete it to get the generated list of the latest
activities.

The profile view is an exception: the server generates it from account data
and does not serve a static file for that path. Names are shown only when
allowed by the account's display options; email and credentials are omitted.

The addresses are ArenaFramework's own, the same in every Arena game:

| Page | Path |
|------|------|
| News (Arena menu) | `n-gage/servlets/msg` (no file extension) |
| Help | `n-gage/device/global/support/support_main.jsp` |
| About | `n-gage/device/global/support/legal_main.jsp` |
| Profile (generated) | `n-gage/device/global/customize/profile/profile_view.jsp` |
| Activity news (Play menu, generated) | `n-gage/device/global/play/activities/news.jsp` |
| Tournament news (generated) | `n-gage/device/global/play/tournaments/news.jsp` |
| Winners (generated) | `n-gage/device/global/play/activities/winners.jsp`, `.../tournaments/winners.jsp` |
| Spotlight (generated) | `n-gage/device/global/play/rankings/spotlight.jsp` |
| Rankings, tournaments, activities, games (generated) | `.../rankings/post_game.jsp`, `post_player.jsp`, `.../tournaments/post_tournaments.jsp`, `.../activities/post_activity.jsp`, `.../games/post_game.jsp` |
| Lists (XML) | `n-gage/device/community/*_x.jsp` (rankings, tournaments, activities and games generated; downloads, skins and sounds empty) |

News, Help and About are the addresses Rifts was seen asking for; the
others come from ArenaFramework's resource files. The server log names
every request (`http: GET ...`) and, at log level 3, the file it served
or the file it looked for (`http: no file ...`).

The example `8053/n-gage/servlets/msg` replaces the News page in Rifts
only; delete it to see the shared one there.

Pages are XHTML Mobile Profile (UTF-8). The Arena menus around them are
black, so the examples set a black background three ways (CSS in the head,
a `style` attribute and the old `bgcolor`/`text` attributes); keep whichever
the N-Gage shows correctly. Files are limited to 64 KB; images (`.gif`,
`.png`, `.jpg`) are served too. Paths may only contain letters, digits,
`-`, `_`, `.` and `/`.
