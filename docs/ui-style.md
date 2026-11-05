# The UI's look

How the M8 screens look ([ADR 0015](adr/0015-ui-sdl2-own-widgets.md) decides how they're built). The code is [`app/ui/theme.h`](../app/ui/theme.h) and [`theme.c`](../app/ui/theme.c); [`app/ui-test`](../app/ui-test/main.c) shows all of it on one screen.

## Where it comes from

The owner's mockups of 2026-10-21: a library screen and a download screen in the style of the original Xbox dashboard, with dark green-black panels, bright lime borders and cut corners. The mockups aren't in the repo, since they show game screenshots and the console maker's logo; this page describes what's taken from them.

Owner decisions of 2026-10-21:

- **The background is the owner's own panorama**, panning slowly from one end to the other and back ([below](#the-background)). The owner made it and released it with the project under its MIT licence ([`app/ui/assets/README.md`](../app/ui/assets/README.md)).
- **No cover art or screenshots for now.** The screens follow the mockups' layout without thumbnails; the detail panel shows text from the catalog (title, year, region, size, the Redump name). Art needs its own source and licence decision, in a later milestone, as M7 decided too ([ADR 0014](adr/0014-catalog-sources-and-format.md)).
- **No logos or marks of the console maker.**

## Colours

| Name (`ui_col_…`) | RGBA | Use |
|---|---|---|
| `bg` | 4, 14, 4 | Behind everything if the background image is missing |
| `text` | 214, 236, 204 | Body text, the selected line |
| `dim` | 134, 172, 122 | Hints, details, sections not selected |
| `accent` | 150, 240, 80 | Titles, the selected game's name |
| `panel` | 6, 22, 8, alpha 210 | Panel fill: the background shows through a little |
| `border` | 80, 190, 40 | Panel borders |
| `highlight` | 40, 110, 20, alpha 235 | The selected line or section |
| `highlight_border` | 160, 255, 70 | Its border |
| `idle` | 16, 46, 12, alpha 225 | The selected line of a list the buttons aren't moving |
| `idle_border` | 48, 104, 30 | Its border |
| `shade` | 0, 0, 0, alpha 150 | Over the screen behind a dialog or the keyboard |

## Fonts

Three bundled fonts (ADR 0015 decision 7, [third_party/fonts](../third_party/fonts/README.md)), all SIL OFL 1.1:

| `ui_font` | Font | Size | Use |
|---|---|---|---|
| `UI_FONT_TITLE` | Michroma | 22 | Screen titles in the title tab, in capitals |
| `UI_FONT_LABEL` | Chakra Petch Bold | 21 | Section names and labels, in capitals |
| `UI_FONT_BOLD` | Noto Sans Bold | 20 | The selected line, the selected game's name |
| `UI_FONT_BODY` | Noto Sans Regular | 20 | List lines, text |
| `UI_FONT_SMALL` | Noto Sans Regular | 16 | Details and hints |

Michroma is wide and geometric like the mockups' titles; Chakra Petch's cut corners match the panels'. Both lack Greek and Cyrillic, so they're for the app's own fixed words only: **anything from outside the app (game names, the catalog, error details) is drawn in Noto Sans.** Picked on 2026-10-21 from a comparison of static OFL fonts (Michroma, Chakra Petch, Audiowide, Share Tech; variable fonts such as Orbitron and Oxanium were out, since nxdk's FreeType 2.4.12 predates them).

## Panels

`ui_panel(d, rect, chamfer, fill, border)`: a translucent fill inside a 2-pixel border, with the **top-left and bottom-right corners cut** at 45° (`UI_CHAMFER`, 12 pixels, for screen panels; 6–8 for rows and buttons inside them; 0 for the footer). It's drawn with fills only, a row at a time across the cut corners, and the fill never paints a pixel twice, which would darken it unevenly.

A selected row or section is a small panel in `highlight` / `highlight_border`, with its text in Bold.

## Layout

All inside the title-safe area (32, 24, 576 × 432), as in ui-test and the library mockup:

| Part | Rectangle | Holds |
|---|---|---|
| Title tab | 32, 24, 240 × 46 | The screen's name in capitals, `UI_FONT_TITLE`, `accent`; in the library 170 wide, to make room for the top panel |
| Top panel | 210, 24, 398 × 46 | Library: the console's and the NAS's addresses, the drives' space; the search while GAMES is searched |
| Sections | 32, 80, 170 × 196 | GAMES, INSTALLED, SETTINGS |
| List | 210, 80, 212 × 320 | The games, one row per line, the selected one highlighted |
| Detail | 430, 80, 178 × 320 | The selected game: name, details, description |
| Footer | 32, 408, 576 × 48 | Status and the buttons' meanings |

## Widgets

The widgets of ADR 0015 decisions 2 and 3, in [`app/ui/widgets.h`](../app/ui/widgets.h) and [`keyboard.h`](../app/ui/keyboard.h). Each is a plain struct the screen owns: `*_input` changes it for one button press, `*_draw` draws it, and both are tested on the PC. ui-test shows them all ([testing.md](testing.md#m8-ui-test-rendering)).

**Buttons** ([`input.h`](../app/ui/input.h)). A chooses, B goes back, Y opens search and Back leaves the app, on every screen. A held D-pad direction presses again after 400 ms, then every 80 ms; no other button repeats, so holding A never chooses twice. The triggers count as pressed past half way and are ready again below a quarter. `input_sdl.c` reads every plugged-in controller.

**List.** One line per item in `UI_FONT_BODY`, 6 pixels apart, cut with "…" when too wide; the highlighted line is a small panel (`highlight`, chamfer 6) in `UI_FONT_BOLD`. While the buttons move something else (`ui_list_draw`'s `idle`: in the library, while Left has moved them to the sections), the line stays marked but dim: `idle` / `idle_border`, its text in `dim`, so only one thing on the screen is bright. Up and Down move a line, LT and RT a page, and the highlight stops at the ends. When not all lines fit, a 4-pixel scroll bar runs down the right edge: the track in `highlight`, the thumb in `highlight_border`.

**Progress bar.** A small panel (chamfer 6) filled in `highlight` from the left, with its label (say "48 %") centred on it in `UI_FONT_BOLD`. `ui_percent` rounds down, so it shows 100 % only once the last byte is in.

**Dialog.** Over the backdrop (the screen behind, darkened by `shade` and standing still, [below](#cost)), a 520-pixel panel centred on the screen: the title in `UI_FONT_LABEL` and `accent`, the message wrapped in `UI_FONT_BODY` (at most six lines), and one to three buttons side by side in `UI_FONT_LABEL`, the highlighted one in `highlight`. Left and Right move between them; A chooses; B gives the choice marked as cancel, if any. Error dialogs (ADR 0015 decision 5) will use it with RETRY, LOG IN AGAIN and BACK, which fit.

**On-screen keyboard.** Over the backdrop, like the dialog, a 520-pixel panel: the field's name in `UI_FONT_LABEL`, the field, 10 × 5 keys and a line of hints.

| Row | Small letters | Capitals | Symbols |
|---|---|---|---|
| 1 | `1234567890` | the same | the same |
| 2 | `qwertyuiop` | `QWERTYUIOP` | `!?#$%^&*()` |
| 3 | `asdfghjkl-` | `ASDFGHJKL-` | `` ~`+=[]{}<> `` |
| 4 | `zxcvbnm.@_` | `ZXCVBNM.@_` | backslash, pipe, then `/;:'",-_` |
| 5 | SHIFT, #+= (ABC on the symbols), SPACE, DELETE, DONE: two columns each | | |

Between them the three layers type every printable ASCII character, so any password can be typed. The D-pad moves and wraps around the edges; on the bottom row it goes key by key. A types the highlighted key, X deletes the last character, Y adds a space, LT switches capitals on and off and RT the symbols (the SHIFT and #+= keys do the same), Start or DONE finishes, B cancels. SHIFT is drawn in `accent` while capitals are on.

The field shows the end of its text when it's wider, with an `accent` caret after it. The keyboard edits the caller's buffer in place, up to its size, and keeps no copy. **A password field shows one dot (U+2022) per character, and its text never reaches the drawing backend**, not even to be measured, so it never sits in a texture or the text cache; the caller wipes the buffer once it's used (ADR 0010 decision 2). A test checks that the backend never sees it.

## Screens

Each screen is a plain struct with `*_input` and `*_draw` like the widgets, tested on the PC; `app/main.c` fills it from core. A dialog on a screen draws over the backdrop, saved in the frame it opens.

### Startup

[`startup_view.h`](../app/ui/startup_view.h): the screens open first thing, and until the library opens the title tab says STARTING, one panel shows "xiso-cartographer <version>" and the newest lines of what startup logs (the drives, the install folder, the credentials, the catalog, the network and the map), the newest in `text` and the ones before in `dim`, and the footer says how to return to the dashboard. A frame is drawn for each line, and the buttons are read meanwhile, so LT + RT + Back + Black works while the map loads. If startup can't go on (no memory, no E: drive, a folder that can't be made), the tab says STOPPED and the panel CAN'T GO ON with the reason above those lines; the app then waits for the dashboard shortcut. nxdk's debug console is used only if the screens themselves can't open (the fonts are missing, say), and for a failed SDL assertion.

### Library

[`library.h`](../app/ui/library.h), in the [layout](#layout) above: the title tab says LIBRARY; the sections; right of it the top panel; under them SUMMARY (HARD DRIVE in INSTALLED), with the number of games listed, installed, and unfinished (failed or stopped in this run, or started in an earlier one); the list of games; the highlighted game's details; and the footer, with a status line (what the last install came to) and the buttons.

- **The details:** the game's picture, if it has one ([art.md](formats/art.md): at most 154 × 130, centred at the top); the name in `UI_FONT_BOLD` and `accent` (two lines at most; the game's own title once its art is read); then in `UI_FONT_SMALL`: the year, region and size in `dim`, separated by " · "; INSTALLED, UNFINISHED, FAILED or STOPPED in `UI_FONT_LABEL`, with what happened or where it is; and a wrapped paragraph in `dim`: who made it, the genres, the release's Redump name and map key, the synopsis and XBE's details, where it comes from (a server's name, or `E:\xcat\isos`), and the overview. White and Black move the paragraph a line up or down when it doesn't fit; the footer's hints end with "White Black more" then.
- **The top panel:** left of its first line `XBOX` and the console's address, right `NAS` and the highlighted game's NAS (else the map's first), its address in `accent` with "off" when its folders didn't list; the labels in `dim`. The second line: each drive's free space, `E:` in `dim`, then a bar of how full it is (`idle` track, `border` for what's used); one drive also says its size and its bar takes the rest of the line. A search replaces the panel while GAMES is searched.
- **Order and long names:** GAMES and INSTALLED are sorted by name (`ui_library_name_cmp`: case folded, digits by value). A highlighted line too wide for its row waits 1.2 s, slides left a pixel every 30 ms until its end shows, waits 1.5 s and starts again (`ui_slide_offset`), drawn whole and cut at the row's text edges; while it waits it ends with "…" like any other. Lines don't slide while the sections have the buttons.
- **Buttons:** the D-pad, LT and RT move in the list. Left goes to the sections, where Up and Down choose one and Right or A come back; the section highlighted while choosing has the bright border, and each list keeps its place. There's no DOWNLOADS section: installs run one at a time, on the install screen, so there's no queue to show (the mockups' DOWNLOADS was dropped on 2026-10-28 until there's a plan for one). In GAMES, A asks "Install <name> to <install folder>?" (INSTALL / CANCEL, B cancels), since the map is a catalog and nothing installs unasked. Back asks "Return to the dashboard?" (LEAVE / STAY).
- **What's installed** ([`xcat/installed.h`](../core/include/xcat/installed.h)): at startup, after each install and after the install folder changes, `main.c` looks in every folder of the install folder (`E:\Games` unless SETTINGS says otherwise). One with a `.xcat-installed` marker is installed; one with a journal and no marker is unfinished; any other folder, from another tool, is left out. A game in GAMES is INSTALLED when a marker's `image=` is its map entry's URL or its file's path, and its note says which folder. It is UNFINISHED when an unfinished folder has a name the install would give it ([ADR 0008](adr/0008-refuse-games-fatx-cant-store.md): the title's FATX name, or numbered); installing it again carries on there.
- **Search:** Y, on any part of the screen but a dialog, opens the [on-screen keyboard](#widgets) on the last search. DONE (Start) lists in GAMES only the games with every word typed somewhere in their name, their facts or the paragraph below (ASCII, without regard to case), and shows GAMES; the highlighted game stays highlighted if it matches. While GAMES is searched, a SEARCH panel right of the title tab shows the words and "3 of 12", and B lists every game again. B in the keyboard leaves the search as it was, and DONE with nothing typed lists every game. When nothing matches, the list says so: Y to search again, B to list them all. INSTALLED isn't searched.
- **SETTINGS:** three lines, Install location, Authentication and Map sources; Up and Down choose one. The details of each line say what it's for.
- **Map sources** ([map.md §9](formats/map.md#9-editing-on-the-console)): A lists "Add source", then every entry and directive of `E:\xcat\map.txt` in order: "archive.org: <identifier>", "Map: <address>", "<title ID>: <file name>", or the line as written. A line's details say what kind it is and its line number, the whole line, and the reader's problem or note for it. A on a line asks "Remove this line from the map?" (REMOVE / CANCEL, CANCEL first). A on "Add source" asks what to add: ARCHIVE.ORG, MAP or IMAGE (B cancels). The keyboard then asks for the item's identifier (ARCHIVE.ORG ITEM IDENTIFIER), the map's address (MAP ADDRESS, starting `https://`) or the image's address (IMAGE ADDRESS, starting `http://`), then its title ID (TITLE ID, LIKE 4D530004). What's typed is kept for the next try at that kind until it's added. A line that wouldn't be valid isn't added, and NOT ADDED says why. After a change, the footer says "Loading the map again...", the game list is rebuilt (the search cleared), and SOURCE ADDED or SOURCE REMOVED says how many games are listed now, with the new line's problem if the reader found one (such as an archive.org item that doesn't exist). B goes back to SETTINGS' lines.
- **Install location** ([settings.md](formats/settings.md), [ADR 0016](adr/0016-settings-file-and-install-location.md)): the details show the folder games install to, and that games already installed stay where they are. A lists `E:\Games`, `F:\Games` and `G:\Games` for the drives the console has, then "Other folder", with the current one highlighted. The details of a folder show its free space, and CURRENT on the one in use. A on a folder makes it the install folder: it's made if needed, saved in `E:\Apps\xcat\settings`, INSTALLED and the marks in GAMES are looked up there again, and a message with OK says so. A on "Other folder" opens the keyboard (INSTALL FOLDER) on the current folder; DONE does the same with what's typed, and a folder that can't be used is refused with the reason (NOT CHANGED). B goes back to SETTINGS' lines. A problem with the settings file is shown in a message when the library opens.
- **Authentication:** the details say whether archive.org is logged in. A lists archive.org first, then every other server the map gets games or included maps from (`xc_map_hosts`), each scheme, host and port once, in the map's order; archive.org's subdomains count as archive.org. A plain-http server is shown as `http://host`, and a port that isn't the scheme's own is shown too. A server's details say how many games (and included maps) come from it, and LOGIN [name] with the credentials section that would be sent to it ([credentials.md §6](formats/credentials.md#6-when-credentials-are-sent)), or NO LOGIN; a plain-http server without one says a login only ever goes to a LAN address with `allow_http`. On a server with a login, A asks "Remove the login [name] from this console?" (REMOVE / CANCEL, CANCEL first), which removes that section and keeps every other byte of the file; a message with OK says LOGIN REMOVED, or why not. On one without, A says how logins are added: by hand, in `E:\Apps\xcat\credentials`. B goes back to SETTINGS' lines.
- **archive.org** (on the Authentication page): the details say LOGGED IN or NOT LOGGED IN (whether the credentials file has an `[archive]` section) and that the password is never stored. Not logged in, A opens the keyboard for ARCHIVE.ORG EMAIL, then ARCHIVE.ORG PASSWORD (dots only). DONE with both typed logs in ([login.md](formats/login.md)) while the footer says "Logging in to archive.org...". A message with OK then says LOGGED IN, or why not: no account with that email, a wrong password, another reason archive.org gave, no network, or a problem with the credentials file. B on either keyboard cancels, and the password is wiped. Logged in, A asks "Log out of archive.org on this console?" (LOG OUT / CANCEL, CANCEL first), which removes the section. The email stays typed for the next login until the app closes.
- **INSTALLED:** those folders, by name without regard to case: the folder's name in the list; in the details its size and number of files, INSTALLED or UNFINISHED with its path, and the title and title ID from `default.xbe`, the checks made, and where the image came from (a server's name, never the whole URL, or the file's path). A does nothing there yet.

### Install

[`install_view.h`](../app/ui/install_view.h): the title tab says INSTALL, one panel fills the middle, and the footer says "B stop". In the panel:
- the game's name, as in the details;
- the phase in `UI_FONT_LABEL` (CONNECTING, CHECKING THE IMAGE, WRITING FILES, VERIFYING) with a line on what it means;
- a progress bar the panel's width with the percentage;
- "1.50 GB of 4.38 GB" (`ui_format_size`: decimal units, three digits, rounded down, like the dashboard);
- "File 3 of 120";
- "8.1 MB/s, about 6 minutes left" (`ui_format_left`), once the phase has run three seconds;
- a status line in `dim` for the network's steps and retries ("The network connection failed. Trying again in 4.0 s (2 of 40)."), cleared as soon as data flows.

B opens STOP / CONTINUE, CONTINUE highlighted. At the end a dialog: INSTALLED with the folder and OK, or NOT INSTALLED with the error, what to do about it (`xc_err_action`, [ADR 0015](adr/0015-ui-sdl2-own-widgets.md) decision 5), then its details, and RETRY / BACK. RETRY is offered only when trying again can help (`xc_err_retry`: the network, the server, free space, a damaged download); otherwise BACK alone. The dialog shows up to six lines; the whole text is in `install.log` and in the game's details. When archive.org refused the login ([login.md §7](formats/login.md#7-logging-in-again)), LOG IN takes RETRY's place: it goes back to the library and opens the keyboards for the email and password. After a successful login the same install starts again at once and carries on where it stopped; if the login fails or is cancelled, nothing carries on.

During an install the background stands still, and the screen is drawn at most four times a second and when a button is pressed, so drawing takes little from the install ([below](#cost)). The screen has a thread of its own (`install_look` in `app/main.c`) that reads the buttons every 50 ms, whatever the install is doing. The install reports progress only once a MiB, which is every half second on a slow download and never while it waits on the network or a disk flush; nxdk's SDL sees a button only if it's still down when the pads are read, so a short press between two reports was lost. While the thread runs it alone calls SDL; the install's callbacks change the screen's state under a lock.

## The background

`app/ui/assets/background.png` is 1440 × 480: the screen's height, and 800 pixels wider than the screen. The XBE build converts it to `ui\background.bmp` next to the XBE (`tools/ui/png2bmp.py`, since nxdk's SDL loads BMP without SDL2_image), and the backend loads it at start. Without it, the screens draw on `ui_col_bg`.

`ui_pan_x(t)` gives the offset: 20 pixels a second (`UI_PAN_SPEED`) from the left end to the right and back, 41 seconds each way. At each end it slows evenly to a stop over one second (`UI_PAN_EASE_MS`) and speeds up again as long, so it never jumps. Owner decision of 2026-10-22: the first version eased over the whole minute-long leg, so near the ends it moved a pixel every half second or so, which looked like dropped frames, and took ages to stop and turn.

## Cost

Every frame is drawn by the console's CPU, 640 × 480 pixels, into the framebuffer that isn't shown (ADR 0015, `draw_sdl.c`). So a screen draws a new frame only when something changed: a button, new text, or the background moving a whole pixel. With the pan that's up to 20 frames a second, slower near the ends. The install screens keep the same rule; if the pan costs installs speed, they can stop it while a download runs (to be measured on hardware in the M8 install screen).

What keeps a frame inside one refresh (16.7 ms) on the console's 733 MHz CPU. The limit is memory more than the CPU: one pass over the screen's 1.2 MB took 8.7 ms on hardware, so the frame makes as few passes as it can.

- **Drawn where it's shown.** The backend has two framebuffers of its own, in cached memory, and draws each frame straight into the one not on screen, then switches at the vertical blank: no copy to the screen. The CPU's cache is written back (`wbinvd`) before the switch, since the GPU reads memory. XVideoSetMode's framebuffer is write-combined, fast to write but very slow to read, and blending reads.
- **No clear.** With the panorama loaded, `ui_sdl_begin` doesn't clear the screen: the frame's `ui_background` or `ui_backdrop` covers all of it.
- **Nothing copied twice.** A screen names its panels to `ui_background_under`, which leaves the background out inside them (clear of their cut corners), since the panels copy the tinted background there anyway. On the library layout that's about 40 % of the screen.
- **Text measured once.** Cutting text to fit and wrapping it measure the same strings every frame; the backend keeps their widths (512 strings up to 128 bytes).
- **Copies around the cache.** Images and the backdrop are copied 64 bytes at a time through the MMX registers, reading ahead with `prefetchnta` and writing with `movntq`, which stores straight to memory instead of first reading each destination line into the cache. Small copies use the CPU's string copy (`rep movsd`).
- **Optimised code.** nxdk compiles without optimisation, and its `memcpy` copies a byte at a time. The build image builds SDL, SDL_ttf and FreeType again at `-O2` ([`docker/nxdk.Dockerfile`](../docker/nxdk.Dockerfile)), and `app/ui/ui-assets.mk` builds the UI's own code at `-O2`. The rest of nxdk is left as it was tested.
- **No blending of large areas.** A translucent panel blended pixel by pixel costs about 200,000 blends a frame for one screen. The backend makes `UI_IMAGE_BACKGROUND_PANEL` at start, the panorama with `panel` blended over it once, and `ui_panel` copies the part behind each panel from it, at the background's pan: the same pixels for the price of a copy. Highlights and small panels are still blended, by the backend's own loop.
- **A still backdrop for dialogs and the keyboard.** `ui_backdrop_save` keeps the screen behind, darkened by `shade` (black at alpha 150), in the frame a dialog opens; `ui_backdrop` copies it back in the frames after, so the screen behind isn't drawn or darkened again. It stands still while the dialog is open.

The first build without these ran at 9 frames a second on the console, 4 with the keyboard open; with optimised code, the tinted panels and the backdrop but still a copy to the screen, at 22–29; drawn in place, at 30–40, the drawing still taking 17–19 ms (testing.md, 2026-10-22); with all of these, at 60, a frame taking 15.6 ms, 6.7 of it copying (2026-10-25). ui-test's footer shows where a frame's time goes: "Change 21/s 1.9+0.0+0.0 ms" is the drawing, the SDL work left when it's presented, and the flip (writing the cache back); then the parts of the drawing: copying images ("copy"), blending, SDL's queued text and borders ("sdl"), measuring text ("size") and rendering text not yet cached ("text").
