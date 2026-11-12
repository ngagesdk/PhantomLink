# Arena device pages

Example pages for the server's `www_root` setting (docs/ARENA.md, "Device
pages"). Copy this folder next to the server, point `www_root` at it and
edit the pages; changes show up on the next request, no restart needed.

A request for `/n-gage/...` is answered with:

1. `www_root/<game class>/n-gage/...` when it exists, for the game the
   user is logged in with (Rifts: Promise of Power is 8053),
2. else `www_root/n-gage/...`,
3. else the server's built-in placeholder (an empty list for `*_x.jsp`,
   a short black page otherwise).

The addresses are ArenaFramework's own, the same in every Arena game:

| Page | Path |
|------|------|
| Support | `n-gage/device/global/support/support_main.jsp` |
| Legal | `n-gage/device/global/support/legal_main.jsp` |
| News | `n-gage/device/global/play/activities/news.jsp` |
| Tournament news | `n-gage/device/global/play/tournaments/news.jsp` |
| Winners | `n-gage/device/global/play/activities/winners.jsp`, `.../tournaments/winners.jsp` |
| Spotlight | `n-gage/device/global/play/rankings/spotlight.jsp` |
| Lists (XML) | `n-gage/device/community/*_x.jsp` (games, rankings, tournaments, ...) |

Pages are XHTML Mobile Profile (UTF-8). The Arena menus around them are
black, so the examples set a black background three ways (CSS in the head,
a `style` attribute and the old `bgcolor`/`text` attributes); keep whichever
the N-Gage shows correctly. Files are limited to 64 KB; images (`.gif`,
`.png`, `.jpg`) are served too. Paths may only contain letters, digits,
`-`, `_`, `.` and `/`.
