# Testing

## Test ladder

Every change climbs as far up this ladder as it affects (see [HANDOFF.md §4](HANDOFF.md#4-strategies)):

1. **Host unit tests:** `ctest --preset host-debug`. Run by CI on Linux and Windows for every pull request.
   - Parser and extractor tests use the synthetic xiso fixtures from [tools/fixtures/](../tools/fixtures/README.md) (small, deep, XGD1/XGD2/XGD3 full-disc, and >4 GiB with `-DXCAT_LARGE_FIXTURES=ON`; CMake generates them at build time). CI checks the fixtures themselves against extract-xiso.
   - Every parser also has a libFuzzer target. CTest replays its seeds on every build, and CI fuzzes each parser for 60 s per run ([CONTRIBUTING.md#fuzzing](../CONTRIBUTING.md#fuzzing)).
2. **Host integration tests:** `xcat-cli` against the fixture images (`extract_*`, `check_*`, `layout_*`) and against the Range test server with scripted faults (`get_*`, `install_*` including installs killed mid-way and resumed, `map_cli`; [tools/testserver/](../tools/testserver/README.md)). Also run by CTest. HTTPS (M5) has its own scenarios against the same server with the test PKI ([tools/fixtures/tls/](../tools/fixtures/tls/make_certs.sh)): `get_https*` (downloads, redirects, resume, an unknown CA, the built-in roots, a wrong name, a reset and a too-late clock, a bad `--ca-file`) and `install_https`.
3. **xemu:** manual for now. Needs your own BIOS/MCPX/HDD files, which can't go in CI. See [build-xbox.md](build-xbox.md#run-in-xemu).
4. **Real hardware:** manual. See [build-xbox.md](build-xbox.md#run-on-hardware).

**From M6 the app installs only what you pick** ([build-xbox.md](build-xbox.md#what-the-app-does)). Where an earlier procedure below says "launch the app", launch it, highlight that procedure's entry in the library and press A, then A again for INSTALL (before M8: press A to see the list, then A on the entry). After a power cut, pick the same entry again to resume.

Steps 3 and 4 are required for every milestone that touches the platform layer. Record each run in the results log below and in the MR description.

## Hardware test bench

| Console | Revision | RAM | HDD | Mod | Dashboard |
|---|---|---|---|---|---|
| Main console | _revision?_ | _64/128 MB?_ | _stock/upgraded?_ | _soft/hard?_ | _dashboard?_ |

## Results log

Newest first. "Build" is the commit or CI artifact tested.

| Date | Milestone | Build | Where | Result | Notes |
|---|---|---|---|---|---|
| 2026-10-28 | M9 title image | branch `feat/title-image` | PC; xemu, no map, run by the agent | ✅ PC, xemu; dashboard ⏳ | `xbe_title_image.py` added `$$XTIMAGE` to the app's `default.xbe` (headers 0x5F9 → 0x75A bytes, 5 → 6 sections, 10,240 bytes at the end). pyxbe, an independent reader, listed the section and decoded a 128 × 128 DXT1 image that matches the logo. The walkthrough disc with this XBE booted in xemu and passed ("Script passed: 63 commands, 19 screens checked"). Not yet seen in a dashboard: step 1 of "M9: title image". |
| 2026-10-28 | M9 dim highlight in xemu | branch `feat/dim-unfocused-highlight` | xemu, no map, run by the agent | ✅ xemu | A scratch UI script (not committed) pressed Left: GAMES had the bright border, and the games list's highlighted line ("small") was drawn dim, its text in `dim`. |
| 2026-10-28 | M8 startup screen in xemu | branch `feat/startup-screen` | xemu, no map, run by the agent | ✅ xemu | No debug console text at startup: the screen is black while the screens open (fonts and images), then the library. Startup with no map takes too little time to see, so a scratch build (not committed) waited 4 s, then stopped: the STARTING screen showed the version and the startup lines, newest bright, with the dashboard shortcut in the footer; then STOPPED, CAN'T GO ON with the reason above them. The walkthrough disc of this build still passed ("Script passed: 63 commands, 19 screens checked"). |
| 2026-10-28 | M8 scripted walkthrough in xemu | branch `feat/ui-script` | xemu, no map (the bundled test image), run by the agent | ✅ xemu | Step 1 of "M8: scripted walkthrough": `app/xiso-cartographer-walkthrough.iso` played the whole walkthrough untouched in about 25 s and ended with "Script passed: 63 commands, 19 screens checked" (read from the guest's framebuffer over QMP). On the way: the search keyboard typing `sma…`, the test image installed, DOWNLOADS, the LEAVE question and SETTINGS' pages. It wrote only what the walkthrough does: the test image's install and `install.log`. With "M8: error messages", the exit test's walkthrough and error messages are met. |
| 2026-10-28 | M8 error messages on hardware | `main` at `2ba6618` (#96) | Hardware: main console, LAN (owner) | ✅ Hardware | Every step of "M8: error messages" (owner: "all tests pass"): a pulled cable ends NOT INSTALLED with the network error, its action and RETRY, and RETRY carries on; a missing file (HTTP 404) and a file that isn't an Xbox image end with their action and BACK alone; `install.log` has a "What to do:" line for each. With `test_bytes`, the M8 exit test's "every error has a message and an action" is met. Not run in xemu (a failing install would have written the xemu disk's `map.txt`). |
| 2026-10-27 | M8 map sources on hardware | `main` at `b865b13` (#94) | Hardware: main console, internet (owner) | ✅ Hardware | Every step of "M8: map sources" (owner: "tested all cases and worked"): the three SETTINGS lines and their details (Authentication explains the page); the map's lines listed; adding an archive.org item, an image and a map, with invalid ones refused (NOT ADDED) and the typed text kept; removing a line; the map and game list loaded again after each change, with every other line kept; a new `map.txt` when there was none. With "M8: install location" and "M8: authentication", the settings part of M8 is done. |
| 2026-10-27 | M8 map sources in xemu | branch `feat/map-sources` | xemu, no map | ✅ xemu | Steps 1 and 3 of "M8: map sources", with timed presses, without adding anything (that would write the xemu disk's `map.txt`): SETTINGS lists three lines, each with details saying what it's for; Map sources lists only "Add source"; the ADD SOURCE question shows ARCHIVE.ORG, MAP and IMAGE, and its keyboard opens and closes. Adding, removing and reloading are covered by `test_map_edit`, `test_ui_library` and `fuzz_map_edit` (34.7M runs); steps 2 and 4–8 need hardware. |
| 2026-10-27 | M8 authentication in xemu | branch `feat/authentication` | xemu, no map | ✅ xemu | Step 1 of "M8: authentication", and A on the page, with timed presses: SETTINGS lists Install location and Authentication; the page lists archive.org alone (xemu's disk has no map), NOT LOGGED IN, "Not in the map"; A opens the ARCHIVE.ORG EMAIL keyboard and B closes it. The other servers' lines, LOGIN [name] and REMOVE are covered by `test_ui_library`; steps 2–6 need hardware with a map. |
| 2026-10-27 | M8 install location on hardware | `main` at `b07bb3a` (#92) | Hardware: main console, F: partition (owner) | ⚠️ Hardware | Installing to `F:\Games` worked, but the install screen's WRITING line said `E:\Games`: the text was fixed in the Authentication PR (step 6 of "M8: authentication" checks it). |
| 2026-10-27 | M8 install location in xemu | branch `feat/install-location` | xemu, stock drive (no F: or G:) | ✅ xemu | Steps 1–3 of "M8: install location", with timed presses: SETTINGS lists Install location and archive.org; the folders are `E:\Games` (5.11 GB free, CURRENT) and Other folder, since the drive has no F: or G:; choosing `E:\Games` saved it; typing `qq` was refused with "must start with E:\, F:\ or G:\". Steps 4–7 need hardware with an F: partition. |
| 2026-10-27 | M8 log in again on hardware | `main` at `6c29b83` (#89) | Hardware: main console, internet (owner) | ✅ Hardware | Steps 1–5 of "M8: log in again" (owner: "worked great"): with `logged-in-sig` spoiled over FTP, installing a login-required item ended with LOG IN; after logging in on the console the stopped install carried on and finished. archive.org answered the bad session in a way the app treats as a refused login (401/403). With the on-box login rows, this meets the login part of the M8 exit test: log in on the Xbox with no PC, the token is saved and the password isn't, a login-required item installs, and an expired or revoked session offers "log in again" and then resumes the install. |
| 2026-10-27 | M8 on-box login on hardware (try 2) | `feat/onbox-login` at `224b5e9` (#88) | Hardware: main console, internet (owner) | ✅ Hardware | "Tried the login again on the box and it works" (owner): logged in to archive.org on the Xbox with the email and password, after logging out in try 1. Steps 5–7 passed too (owner): neither the password nor the email is in `credentials` or `install.log`; an item that needs the login installed; after a restart SETTINGS still said LOGGED IN. This meets the login parts of the M8 exit test (the token is saved, the password isn't, a login-required item installs); the expired-session part ("log in again", then resume) is still to come. |
| 2026-10-27 | M8 on-box login on hardware (try 1) | `feat/onbox-login` at `fdda051` (#88) | Hardware: main console, internet (owner) | ❌ Fail | Logging out worked ("[archive] removed", then 0 sections). Two logins ended "The login didn't get through", logged as `Login failed: The image is damaged or invalid (HTTP 200)`: archive.org answered, but the answer was refused as corrupt. The official tooling cuts each cookie at its first `;`, so the answer gives them in Set-Cookie form (`value; path=/; domain=...`), and the `;` and spaces failed the cookie-octet check. Fixed: only the value before the first `;` is kept (login.md §3). The log and the message now say what was wrong with an answer, and no longer use the image error's words. |
| 2026-10-27 | M8 on-box login, steps 1 and 3 in xemu | `feat/onbox-login`, and a scratch build pressing Left, Down ×3, Right, A, "m", "e", Start, A ×3, B on a timer (not committed) | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run | Read from the guest's framebuffer over QMP; no login was sent. SETTINGS showed archive.org, NOT LOGGED IN, "A logs in with your archive.org email and password" and the note that the password is never stored. A opened ARCHIVE.ORG EMAIL with the typed text; Start opened ARCHIVE.ORG PASSWORD, which showed three dots for three letters. The image has no credentials file. |
| 2026-10-27 | M8 search on hardware | `main` at `a6bd4a3` (#86) | Hardware: main console (owner) | ✅ Hardware | "On box tests work" (owner): the search steps of "M8: search" pass. |
| 2026-10-26 | M8 search, steps 1 and 2 in xemu | `feat/ui-search`, and a scratch build pressing Y, s, m, Start on a timer (not committed) | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run | Read from the guest's framebuffer over QMP, on the HDD image used for the earlier tests. Y opened the keyboard over the library, with SEARCH and "s" in the field after the first key. After Start, the SEARCH panel said "sm" and "1 of 1", GAMES listed the sample (still INSTALLED), and the footer said "A install  B list all  Y search". The image lists one game, so narrowing a longer list is left to hardware. |
| 2026-10-26 | M8 installed games on hardware | `feat/ui-installed` at `92a5f7f` (#85) | Hardware: main console, internet (owner) | ✅ Hardware | "Works" (owner): games installed in earlier visits keep their INSTALLED mark after a restart, and the INSTALLED section lists them. |
| 2026-10-26 | M8 installed games, steps 1 and 2 in xemu | `feat/ui-installed`, and a scratch build pressing Left, Down, Down, Right on a timer (not committed) | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run | Read from the guest's framebuffer over QMP, on the HDD image used for the earlier tests. GAMES marked the bundled sample INSTALLED, "In E:\Games\small", from an install in an earlier session; SUMMARY said "1 game listed, 1 installed". INSTALLED listed "small" and "small (2)" (both earlier installs of the sample): "197 KB \| 9 files", INSTALLED, "E:\Games\small", "Checked: every file's CRC-32. From E:\xcat\isos\small.iso." The synthetic sample has no title ID. |
| 2026-10-26 | M8 library and install screens, step 5 again on hardware | `feat/ui-library` at `3054ff3` (#84) | Hardware: main console, internet (owner) | ✅ Hardware | "Flawless" (owner): with the buttons read on the install screen's own thread, stopping and retrying answers every press. Noted for later: after a restart the library no longer marks games installed in an earlier visit. As designed for now (the marks and THIS VISIT cover one visit); finding installed games in E:\Games belongs to the INSTALLED GAMES section, a later M8 change. |
| 2026-10-26 | M8 library and install screens, all steps on hardware | `feat/ui-library` at `9b9ae1a` (#84) | Hardware: main console, internet (owner) | ❌ Step 5; the rest ✅ | "All steps worked except" stopping: the first stop (B, then STOP) and RETRY worked, and the install resumed 320 MiB in. The second stop, in the retried install, opened STOP / CONTINUE and then ignored the buttons for a while before it let the owner stop. `install.log` showed the install running all along, reporting progress every 1 MiB, which past the first 387 MiB of a zip (the part skipped at ~26 MB/s) is only every half second at ~2 MB/s. The buttons were read only then, and nxdk's SDL sees a button only if it's still down when the pads are read: short presses fell between two reads. The first stop was in the fast part, so it answered. Fixed in the next build: the install screen has its own thread reading the buttons every 50 ms. An earlier full install of another zip, in the same log: 494 files, 7462 MiB at 2.0 MB/s, read back at 16.3 MB/s, matched the catalog. |
| 2026-10-26 | M8 library and install screens, steps 4 and 6 on hardware | `feat/ui-library` at `4c50b05` (#84) | Hardware: main console, internet (owner) | ✅ Install screen | "The install screen worked as expected", averaging **2 MB/s**, against 2.2 MB/s for the M6 run of a game from the same source (2026-10-20): drawing the screen four times a second costs the install little, if anything. Steps 2, 3, 5, 7 and 8 (moving in the library, leaving, stopping, a pulled cable, a failure) not reported yet. |
| 2026-10-25 | M8 library and install screens: steps 1, 3–6 in xemu | `feat/ui-library`, and scratch builds pressing buttons on a timer (not committed) | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run | Read from the guest's framebuffer over QMP. The library listed the bundled sample with "No catalog loaded. From E:\xcat\isos on this console."; the sections, "Not here yet", LEAVE / STAY, and INSTALL / CANCEL worked. A real install of the sample went through the install screen to "INSTALLED: In E:\Games\small, every file read back and checked." A second build replaced the install with made-up progress to show the rest: WRITING FILES with "13 %", "574 MB of 4.38 GB", "File 23 of 120", "8.1 MB/s, about 8 minutes left"; STOP / CONTINUE; NOT INSTALLED with "Not enough free space on the drive. It needs 4.38 GB; 1.20 GB is free." and RETRY / BACK; then the game marked FAILED, "1 unfinished". The first try cut "NOT INSTALLED" in the details panel: now FAILED or STOPPED. |
| 2026-10-25 | M8 UI test (widgets), step 8 with the third speed fix | `feat/ui-widgets` at `ac2b472` (#83) | Hardware: main console, with a controller (owner) | ✅ Hardware | On change 20 frames/s, "14.4+1.2+0.0 ms: copy 6.7 blend 0.6"; every frame **60 frames/s**, "14.5+1.1+0.0 ms: copy 6.6 blend 0.7". A frame takes 15.6 ms of the 16.7 a refresh allows: it fits, with little to spare. Copying the background and panels is still most of it. The keyboard is "smooth and works great" (owner); its frame times can't be read, since the footer is part of the still screen behind it, as designed. The pan "looks a lot better" at the ends. |
| 2026-10-22 | M8 UI test (widgets), the third speed fix and the pan: steps 1, 2, 5, 6, 7 in xemu | `feat/ui-widgets` with the fix (#83), and the scratch build pressing buttons on a timer | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run (step 8) | Looks as before. The footer's split showed measuring text at 1.1 of 3.0 ms; with widths kept, 0.1, and a frame 1.8–1.9 ms ("Change 21/s 1.9+0.0+0.0 ms: copy 0.7 blend 0.0 sdl 0.4 size 0.1 text 0.0"). A dialog's first frame now shows the screen behind darkened; before, for that one frame, it was drawn over the screen as it was. The pan's new timing is checked by `test_ui_theme`, not by eye. |
| 2026-10-22 | M8 UI test (widgets), step 8 with the second speed fix | `feat/ui-widgets` at `a1cf71d` (#83) | Hardware: main console, with a controller (owner) | ❌ Hardware (frame rate) | On change 22 frames/s, every frame 30–40: "Draw 17–19 (copy 8.7 blend 0.6) finish 1.2 flip 0". The flip, writing the cache back, costs nothing, but the drawing still doesn't fit in 16.7 ms with finish, and 8–10 ms of it was outside copying and blending. Fixed in the next commit: the background isn't copied under the panels (`ui_background_under`), text widths are kept instead of measured every frame (in xemu measuring was a third of the frame), and the footer splits the drawing further (SDL's text and borders, measuring, rendering). Also, the owner: near the ends the pan "looks stuttery like it is dropping frames" and takes too long to stop and turn. Its easing spread over the whole minute-long leg, so near an end the panorama moved a pixel every half second or so. Now it moves at a steady 20 pixels a second and eases over one second at each end (docs/ui-style.md#the-background). |
| 2026-10-22 | M8 UI test (widgets), the second speed fix: steps 1, 2, 5, 6, 7 in xemu | `feat/ui-widgets` with the fix (#83), and the scratch build pressing buttons on a timer | xemu v0.8.139 on the owner's PC, run by the agent | ✅ xemu; hardware to be run (step 8) | The screen, the dialog and the keyboard look as before, with nothing left over from the frame before last in either framebuffer. The footer's new split: "Draw 2.7–3.3 (copy 0.5–0.9 blend 0.0–0.6) finish 0.0–0.1 flip 0.0". xemu doesn't model the console's memory, so only hardware can show what the fix is worth. |
| 2026-10-22 | M8 UI test (widgets), step 8 with the first speed fix | `feat/ui-widgets` at `4cc8aa3` (#83) | Hardware: main console, with a controller (owner) | ❌ Hardware (frame rate) | On change 22–24 frames/s, every frame 28–29: "Draw 24 + finish 1.2 + copy 8.7 ms", 34 ms a frame against 16.7. Better than 9, still not acceptable. Cause: memory, not the CPU. Copying the finished frame to the screen, one pass over 1.2 MB, took 8.7 ms; the drawing made about three more passes (clearing the screen, the panorama, the panels over it), each reading every destination line into the cache before writing it. Fixed in the next commit: frames drawn straight into two cached framebuffers (no copy to the screen; the cache is written back before the switch), no clear under the panorama, and the large copies by MMX with `prefetchnta` and `movntq`, which write around the cache. The footer splits "Draw" into copying and blending, and "copy" became "flip" (docs/ui-style.md#cost). |
| 2026-10-22 | M8 UI test (widgets), the speed fix: steps 1, 2, 5, 6, 7 in xemu | `feat/ui-widgets` with the fix (#83), and the scratch build pressing buttons on a timer | xemu v0.8.138 on the owner's PC, run by the agent | ✅ xemu; hardware to be run (step 8) | The screen looks the same as before. The whole frame now measured: "Draw 3.5–3.6 + finish 0.1 + copy 0.4 ms" on the list, and 3.5 + 0.0 + 0.4 ms with the dialog or keyboard open. The dialog and keyboard draw over the darkened, still screen; the password field still shows only dots. xemu's CPU is faster than the console's, so the hardware figures decide. |
| 2026-10-22 | M8 UI test (widgets), steps 1–8 | `feat/ui-widgets` at `7184e3c` (#83) | xemu; Hardware: main console, with a controller (owner) | ✅ xemu, ❌ hardware (frame rate) | **xemu:** on change, "2 ms to draw" at 22–26 frames/s; with White, 59–60 frames/s; with the keyboard open, "4 ms", and 30 frames/s with White. **Hardware:** everything works and fits the TV, LT and RT page the list and switch the keyboard's layers, but only **9 frames/s** ("7 ms to draw"), White or not, and **4 frames/s** ("11 ms") with the keyboard: not acceptable (owner). The "ms to draw" only timed queuing SDL's commands; the pixels were drawn when the frame was presented, untimed. Cause: nxdk builds SDL, SDL_ttf and FreeType without optimisation (and its `memcpy` copies a byte at a time), and every translucent panel was blended pixel by pixel through it, about 200,000 pixels a frame, 500,000 more with the keyboard. Fixed in the next commit: SDL, SDL_ttf and FreeType built at `-O2` in the build image and the UI code at `-O2`; panels copied from a tinted copy of the panorama made at start; the screen behind a dialog or the keyboard saved once, darkened, and copied; blending and copies done by the backend's own loops. ui-test now times the whole frame (docs/ui-style.md#cost). |
| 2026-10-21 | M8 UI test (widgets), steps 1, 5, 6 and 7 in xemu | `feat/ui-widgets`, with a scratch build that pressed a fixed list of buttons on a timer (not committed) | xemu v0.8.138 on the owner's PC, run by the agent | ✅ Steps 1, 5, 6, 7 (scripted presses); 2–4 and 8 remain for the owner | Read from the guest's framebuffers over QMP. The list paged with RT and showed its scroll bar; the dialog drew over the dimmed screen and returned DETAILS; the keyboard typed `` qwS ` `` with capitals, a space and a symbol, and Start returned it; the password field showed 5 dots, B cancelled and the footer reported 5 characters, wiped. 2–5 ms to draw a frame, the keyboard's the slowest. Held-button repeat and the triggers need a real controller (steps 3 and 6). |
| 2026-10-21 | M8 UI test (rendering), steps 1 and 3 in xemu | `feat/ui-draw` after the fix below (#80) | xemu v0.8.138 on the owner's PC, run by the agent | ✅ Steps 1 (screen), 3 not run (needs a controller press) | Steady for the whole capture; the bottom line updates every second: "On change: last frame 1–3 ms to draw", the text cache's misses grow only by the changing stats line. Checked by reading the guest's framebuffers and the CRTC register over QMP, and by captures of the xemu window. Steps 2–4 remain for the owner. |
| 2026-10-21 | M8 UI test (rendering), first two builds | `feat/ui-draw` at `ee77d03` and `7e2e00e` (#80) | xemu; Hardware: main console | ❌ xemu, ✅ hardware | **Hardware:** the owner reports it works, **60 frames/s** with Y. **xemu:** black, with the test screen for a second or two every few seconds; the "59 FPS" was xemu's own counter. Cause: the XBE rebooted after its first frame. A batch of SDL render commands that starts with a clip rect fails `SDL_assert(viewport != NULL)` in nxdk's software renderer (SDL 2.0.10's code), and SDL's assertion handler exits, which on nxdk is a quick reboot. Found by reading the framebuffers over QMP: the frame never changed past "measuring...", and an assertion handler printed the message. Why the console didn't stop at the same assertion wasn't determined. Fixed in `draw_sdl.c` (viewport before every clip rect; failed assertions shown on screen). |
| 2026-10-20 | M7 exit test, step 4 (the catalog on the console) | CI artifact of #77 (`feat/app-catalog`) | Hardware: main console, internet to archive.org | ✅ **M7 exit test met** | `build/catalog/catalog.bin` (`c4ca03c1…`) in `E:\Apps\xcat\` with the M6 `map.txt`. The owner reports the step passes and "is working great": the list is named from the catalog. The counts the procedure asks for (games named, the "matches the catalog" line, the fallback with the file renamed) weren't reported in detail. |
| 2026-10-20 | M7 exit test, steps 1–3 (catalog pipeline) | `main` at #76 (`b28440d`) | Host: Windows 11 (Python 3.15); Docker `python:3.12-slim` (Linux, read-only cache) | ✅ Pass; step 4 (hardware) ⏳ | First snapshot fetched on 2026-10-20 (Wikidata; Redump datfile of 2026-06-14, 27 list pages, 2,683 disc pages): **1,074 games, 2,213 releases, 327 KB**. Both builds printed `catalog.bin sha256 c4ca03c1…` and matched `inputs.lock`; a third, from the cache copied to `%USERPROFILE%\xcat-catalog-cache`, too. CI ran `test_catalog`, `fuzz_catalog`, `catalog_cli_*` and `catalog_pipeline` on every PR from #74 (fuzzing locally: 35 M runs in 5 min, no crash). SOURCES.md and CREDITS.txt reviewed. Reviewing the first build found two pipeline bugs, fixed in #76: Wikidata names under `mul` only (57 games missing, Halo among them) and regional titles of one game split into several. |
| 2026-10-20 | M6 exit test (archive.org source) | CI artifact of `fix/picker-screen` (#70) | Hardware: main console, internet to archive.org | ✅ **M6 exit test met** | `map.txt` held `@archiveorg` with the owner's item plus a non-existent identifier. **The item listed 137 games**, and the non-existent one was a line error: "archive.org item xcat-no-such-item-test doesn't exist or was removed". "Catalog: 137 game(s)"; nothing installed until one was picked. **The list draws correctly** after #70 (before it, pages with long names overflowed the debug console and showed only their tail). Picked a full-disc **Zip64 Deflate** game (one 7,462 MiB `.iso`, 153 files, 1,435 MiB of them): "archive.org says: 6676847145 bytes", and the size check passed. 1 pass, 0 MiB read again, 150 files from a spool that peaked at 2,730 MiB, 7,462 MiB of image in 3,524 s (**2.2 MB/s**; 3.3 and 2.9 MB/s in the M5b runs), TLS 1.2 ECDHE-ECDSA-CHACHA20-POLY1305. Read back 1,434 MiB in 85 s (17.6 MB/s), "OK: 153 files … verified". **Title ID 4B4E001A read from `default.xbe`**, and the name matched the game. **The game launches from the dashboard.** |
| 2026-10-20 | M5b exit test, steps 2–3 (power cut, resume from the zip) | main CI artifact (`baf5a03`) | Hardware: main console, internet to archive.org | ✅ **M5b exit test met** | A second full-disc **Zip64 Deflate** zip (one 7,462 MiB `.iso`, 59 files, 218 MiB of them) from the owner's archive.org account over HTTPS, TLS 1.2 ECDHE-ECDSA-CHACHA20-POLY1305. **Switched off at the power button at 1,155 MiB** (354 s in, 3.5 MB/s). Relaunched: the progress picked up at 1,152 MiB, "continued 1152 MiB into the image", and the rest streamed in one pass: 6,310 MiB of image in 2,262 s (**2.9 MB/s**, steady at 2.6–2.8), **1 pass, 0 MiB read again, all 59 files copied out of a spool that peaked at 3,350 MiB**. This game's directory tables come only after ~3,740 MiB, so nothing was known before the cut ("59 written, 0 kept"): the resume rested entirely on the saved state, the spool files and the zip checkpoint. The member's CRC-32 covered every byte across both attempts, read back 217 MiB in 12 s (17.5 MB/s), "OK: 59 files … verified (resumed)". **The game launches from the dashboard.** Step 4 (a link into the zip) wasn't run on hardware; the map tests cover it. The free space at the start wasn't recorded. |
| 2026-10-20 | M5b exit test, step 3 without the power cut (install from a zip) | main CI artifact (`9bbd602`) | Hardware: main console, internet to archive.org | ✅ Install passes; ⏳ resume not yet run | A full-disc **Zip64 Deflate** zip (one 7,462 MiB `.iso`) from the owner's archive.org account over HTTPS: login from `E:\Apps\xcat\credentials`, time from NTP, built-in roots, TLS 1.2 ECDHE-ECDSA-CHACHA20-POLY1305. **1 pass, 0 MiB read again, 30 of 41 files copied out of a spool that peaked at 1,590 MiB** (the rest written as they streamed past). The whole member was decompressed and checked by its CRC-32: 7,462 MiB of image in 2,328 s (**3.3 MB/s** of image; ~27 MB/s in the first seconds, then steady at 3.2–3.4 MB/s). Read back 1,931 MiB of game files in 114 s (17.7 MB/s), "OK: 41 files … verified". **The game launches from the dashboard.** No power cut this run ("0 kept from an earlier attempt"), so the resume part of the exit test is still open. Earlier attempts failed with "Access denied" because the file was named `credentials.txt`, which the app ignored silently; fixed in #61. The progress showed "file 0 of 0" for the first ~640 s, until the last directory table had streamed past. |
| 2026-10-19 | M5 exit test, step 4 (clock reset, NTP unreachable) | main CI artifact (`b578f44`) | Hardware: main console | ✅ Rejected | Console clock 2001-10-19, earlier than the build. "HTTPS isn't available: Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)", the clock-reset hint, and the entry "NOT INSTALLED" with the same reason. `ntp.txt` held `ntp=192.0.2.1` (the net-test key form) instead of `192.0.2.1`, so the server name was `ntp=192.0.2.1` and NTP failed on its name lookup after 1 request, not after three unanswered requests. The outcome the step checks (no NTP time, clock too early, so no HTTPS) is the same. |
| 2026-10-19 | M5 exit test, step 3 (clock ahead, NTP off) | main CI artifact (`b578f44`) | Hardware: main console | ✅ Rejected | Console clock 2032-10-19. "Time from the console's clock (not earlier than this build)", built-in roots, then "NOT INSTALLED: The server's certificate isn't valid at the current time (is the clock right?)". `ntp.txt` held `ntp=off` instead of `off`, so NTP wasn't switched off but failed on a name lookup (1 request); with no NTP time the app fell back to the console's clock exactly as with NTP off. |
| 2026-10-19 | M5 exit test, step 2 (bad certificate) | main CI artifact (`b578f44`) | Hardware: main console, LAN certificate as `E:\Apps\xcat\cacert.pem` | ✅ Rejected | "Trusting the 1 certificates in E:\Apps\xcat\cacert.pem", time from NTP (stratum 2, 180 ms, clock +1 s off), then "NOT INSTALLED: The server's certificate isn't trusted (unknown issuer or wrong name)". |
| 2026-10-18 | M5 exit test, step 1 (install over HTTPS) | main CI artifact (`b578f44`) | Hardware: main console, internet to archive.org | ✅ Pass | Randomness 256 samples, 256 bits credited, 161 of 256 low bytes seen, seed file read and renewed. Time from NTP (pool.ntp.org): stratum 2, round trip 75 ms, 1 request, console clock −1 s off. Built-in roots (121). **TLS 1.2, ECDHE-ECDSA-CHACHA20-POLY1305.** 3,423 files, 2,332 MiB written in 1,660 s (**1.4 MB/s**, ~1.8 MB/s while the large files streamed), read back in 140 s (17.4 MB/s), verified. **Image MD5 `5cb287d42385d709a4b84b5be78eb218`, SHA-1 `691d618b7fb27de573aa277b0597ef124cf8dd2c`: both equal archive.org's metadata for the file.** No power cut this run (0 files kept from an earlier attempt). Reading the directory tables took ~270 s: archive.org answered `HTTP 500` 36 times, each retried within 1–2 attempts (a few handshakes took ~1.1 s). Launch from the dashboard not reported. |
| 2026-10-17 | M5 (TLS speed, step 1 with a 336 KB window) | #48 CI artifact (`7970756`) | Hardware: main console, internet to archive.org | ✅ Above the 1 MB/s target; the window wasn't the limit | Link 100 Mbit/s full duplex. **TCP handshake 57 ms** (one round trip). First 256 MiB at **1.8 MB/s** (two `HTTP 500`s retried; ~1.8 MB/s steady after the first minute), longest pause 0.2 s, CRC-32 `e3673fc8` as before. Window scaling ours 3, theirs 7. `TCP:` window 335 KB, frames in 188,297, refused by lwIP 0, segments in 188,238, dropped 1, out of memory 0, **receive buffers in use at most 244 of 384**. `Reading:` waited for data 75% of the time, 40,857 reads, window closed at 4, **out-of-order data at 35,932** (88%), unread 52 KB on average. A 335 KB window at 57 ms allows ~5.9 MB/s, so doubling the window (1.7 → 1.8) disproved the window theory of the previous run. The driver never ran short of buffers and lwIP refused nothing, while out-of-order data was queued at almost every read: segments go missing before they reach lwIP, and the server's congestion control holds the rate down. M5's target (≥ 1 MB/s) is met; more speed (two connections, ADR 0006; the driver's error counters, which nxdk compiles out) is left for later. |
| 2026-10-17 | M5 (TLS speed, step 1 with frames and link) | #48 CI artifact (`31e8a4e`) | Hardware: main console, internet to archive.org | ⚠️ Faster, but the window is the limit again | Link **up, 100 Mbit/s, full duplex**. First 256 MiB at **1.7 MB/s** (steady from the first minute, no `HTTP 500`), longest pause 0.2 s, CRC-32 `e3673fc8` as before. `TCP:` frames in 188,271, **refused by lwIP 0**, segments in 188,238, dropped 1, out of memory 0. `Reading:` waited for data 79% of the time, 102,484 reads, window closed at 24, **out-of-order data at 16,045** (16%), unread 6 KB on average. So lwIP's mailbox never overflowed and the link negotiated properly. Segments still go missing on the way (out-of-order data at one read in six), but at 1.7 MB/s with the app idle most of the time, one 167 KB window per round trip fits a ~98 ms round trip under load, so the window is the limit again. (A second run, stopped after 5 s, also showed 1.5 MB/s.) Next: double the window (240 segments, ~336 KB, scale 3) and the driver's receive buffers (384), and log the TCP handshake time and the most receive buffers in use. |
| 2026-10-17 | M5 (TLS speed: is it the 100 Mbit/s link?) | — (PC, curl) | PC on the same home network, wired, adapter forced to **100 Mbps Full Duplex** | ✅ The path is fine at 100 Mbit/s | `curl -r` of the same first 256 MiB from archive.org: **8.0 MB/s** average including a 2 s redirect, ~11.7 MB/s at the end (the link's limit). At auto-negotiated 1 Gbit/s: 14.2 MB/s. So the step down to 100 Mbit/s and the path from archive.org don't lose enough to explain the console's 1.1 MB/s: the loss happens on the console (its driver, lwIP, or how its link negotiated). Next: the network test shows the link's speed and duplex, counts the frames lwIP refused (its input mailbox full, which nxdk drops without counting), and counts reads that found out-of-order data. |
| 2026-10-16 | M5 (TLS speed, step 1 with `Reading:`) | #47 CI artifact (`a33e0a7`) | Hardware: main console, internet to archive.org | ⚠️ Same speed; **the console isn't the limit** | First 256 MiB at **1.1 MB/s** (one `HTTP 500` retried), longest pause 0.5 s, CRC-32 `e3673fc8` as before, lwIP dropped 2 segments, out of memory 0. `Reading:` over the whole download: **the app waited for data 85% of the time** (the same in every minute), 54,234 reads, the window closed at only 30 of them, **20 KB unread on average** of the 167 KB window. The 15% busy time fits the 6.7 MB/s the console decrypts on the LAN (1.1 / 6.7 = 16%). Sampled before printing, the free window was 159, 167, 165 and 76 KB, so the earlier near-zero samples came from the log flush. **Out-of-order data was queued at 3 of the 4 samples**, which points at segments lost on the way (or reordered); each loss makes the server slow down, while the PC gets 18.7 MB/s from the same server. So the data comes too slowly: the window is no longer the limit, packet loss is. Next: find where the loss happens, at the console's 100 Mbit/s port or in its driver. |
| 2026-10-16 | M5 (TLS speed, step 1 with `TCP now:`) | #47 CI artifact (`f16715d`) | Hardware: main console, internet to archive.org | ⚠️ Same speed; the 64 KB theory is wrong | First 256 MiB at **1.1 MB/s average** (one `HTTP 500` retried), longest pause 1.9 s, CRC-32 `e3673fc8` as before. `TCP now:` at 10, 70, 130 and 190 s: **window scaling on** (ours 2, theirs 7), SACK on, MSS 1432. So the 167 KB window is in effect. Our free window at those samples: 26,143, 3,861, 1,045 and 1,398 bytes; advertised: the same, then **0** twice. Out-of-order data was queued at 10 and 70 s. A full window means data the app hadn't read yet. But each sample was taken just after a progress line had been written and flushed to disk, while the app read nothing, so it can't tell whether the console is the limit or whether that pause filled the window. Next: measure over the whole download how long the app waits for data and how full the window is whenever it reads. |
| 2026-10-16 | M5 (TLS speed, step 1 after TCP tuning) | #46 CI artifact (`736796d`) | Hardware: main console, internet to archive.org | ⚠️ Better, at the target's edge | Same first 256 MiB: **1.0 MB/s average** (steady ~1.1 MB/s after a first `HTTP 500` from archive.org, retried once), up from 0.3. Longest pause 0.4 s (was 2.1), CRC-32 `e3673fc8` same as before. `TCP:` window 167 KB, segments in 188,412, dropped 1, out of memory 0. (`frames in 0` is wrong: nxdk's driver doesn't update lwIP's link counters. `out` excludes plain ACKs.) The stalls are gone, but the flat ~1.1 MB/s is what a **64 KB** window gives at ~60 ms, so the larger window probably isn't in effect (window scaling not agreed, or another 64 KB limit). Next: report the negotiated window scale and advertised window. |
| 2026-10-16 | M5 (TLS speed, step 2) | #45 CI artifact (`ea658d9`) | Hardware: main console, LAN HTTPS from the PC (`make_lan_cert.sh` certificate as `E:\Apps\xcat\cacert.pem`) | ✅ Pass | 1 GiB test file over HTTPS at a steady **6.7 MB/s** (plain HTTP on the same LAN: 11.8 MB/s), CRC-32 `6775e853` matches, 0 retries, longest pause 0.1 s. Connected (TCP + TLS handshake) in 252 ms; ECDHE-ECDSA-CHACHA20-POLY1305. **The console's TLS path is well above the 1 MB/s target**; step 1's 0.3 MB/s comes from its TCP over the internet path, not from TLS. Two earlier attempts failed with "TLS error 3": a plain-HTTP `rangeserver.py` from an earlier test still shared port 8080 (Windows allowed both to bind) and answered instead. |
| 2026-10-16 | M5 (TLS speed, step 1) | #45 CI artifact (`ea658d9`) | Hardware: main console, internet to archive.org | ⚠️ Works, too slow | HTTPS to archive.org works end to end: TLS 1.2 handshake and certificate check with the built-in roots, then the redirect to the storage server, connected in 2,113 ms. Server chose **ECDHE-ECDSA-CHACHA20-POLY1305** (0xcca9). **CPU, no network: ChaCha20-Poly1305 18.0 MB/s, AES-128-GCM 3.7 MB/s**. Randomness: 256 samples, 255 bits credited, 127 of 256 low bytes seen, 1.41–1.47 M cycles per sleep. Clock OK. First 256 MiB of the test item at a steady **0.3 MB/s**, 0 retries, longest pause 2.1 s, several 5 s intervals with almost no progress. The PC gets **18.7 MB/s** from the same storage server over one connection (curl, ~60 ms round trip). So the CPU (18 MB/s) and the server aren't the limit: the console's TCP over a long path is. Its 64 KB window without scaling caps one connection at ~1.1 MB/s at 60 ms, and the stalls suggest lost packets recovered by timeouts (lwIP has no SACK). Below the 1 MB/s target until the TCP settings are fixed. |
| 2026-10-15 | M4 (follow-up) | #41 CI artifact | Hardware: main console | ✅ Pass | **LT + RT + Back + Black** returns to the dashboard from the app ([build-xbox.md](build-xbox.md#what-the-app-does)). |
| 2026-10-15 | M4 (attempt 2) | #41 CI artifact (`3abd5d6`) | Hardware: main console, LAN to the PC's `rangeserver.py` | ✅ Pass | `large` fixture regenerated with its file at the FATX maximum, 4 GiB − 16 KiB (fatx-names.md §4b). Power cut at ~1.2 GB; the next start **continued the big file from its checkpoint at 1,216 MiB** ("1 file(s) continued from a checkpoint"), wrote the remaining ~2,880 MiB at a steady ~8 MB/s, read all 4,096 MiB back at ~18 MB/s, and ended "OK: 3 files in E:\Games\large, verified (resumed)", "Done: 1 of 1". **M4 exit test met:** an over-4 GB image installs over the LAN and resumes after a power cut. The speed shown after the resume, and the final MB/s, were miscalculated (corrected after this run). |
| 2026-10-15 | M4 (attempt 1) | #41 CI artifact (`13cf96c`) | Hardware: main console, LAN to the PC's `rangeserver.py` | ❌ Fail | `large` fixture over HTTP at **8.0 MB/s** (network and disk writes together). (1) A power cut at ~1.3 GB restarted the install from 0: the journal only recorded finished files, and the image is almost all one 4 GB file. Fixed by checkpoints every 64 MiB ([ADR 0011](adr/0011-checkpoints-inside-large-files.md)). (2) The second run failed with "Read or write failed" in the last 16 KiB of the 4,294,965,248-byte file. The same install verifies on a PC; the FATX probe then showed the console can't grow a file past 4 GiB − 16 KiB. Fixed by refusing such files (fatx-names.md §4b) and shrinking the fixture's file to exactly that size. |
| 2026-10-15 | M3 (step 3) | #35 CI artifact (`e6b5977`) | Hardware: main console (see bench), same LAN and PC | ✅ Pass | Same 1 GiB file. Network cable pulled at ~31 s (~340 MiB) and plugged back in: no data for 15 s, then "The server stopped responding, retrying in 0.9 s", and the download **resumed at ~347 MiB, not 0**. Finished in 108 s (9.9 MB/s average including the outage), 2 retries, longest pause 16.3 s, **CRC-32 `6775e853` matches**. The cable was back in before the first retry, so the reconnect worked on the first try; a longer outage (several failed connects with backoff) wasn't exercised. **M3 exit test met** (1 GB over the LAN, CRC match, MB/s recorded, survives a cable pull). Side note: in 2 of the 4 runs today, the very first request after startup timed out once (15 s) before working on the retry; worth a look before M4. |
| 2026-10-15 | M3 (step 2) | #35 CI artifact (`c208e95`) | Hardware: main console (see bench), LAN to a Windows PC running `rangeserver.py` | ✅ Pass | 1 GiB test file (`make_test_file.py --size 1G`) downloaded twice: **1,073,741,824 bytes in 90 s, 11.8 MB/s average** (steady 11.8 MB/s throughout), 0 retries, CRC-32 `6775e853` matches both times. About the same as the console's FTP upload (~11.7 MB/s), so the HTTP client isn't the bottleneck. A power cut mid-download restarted the test from 0 on the next boot, as expected: the test keeps progress in memory only (resume across power cuts is M4). |
| 2026-10-14 | M2 (step 3) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | One of the owner's retail games, full-disc dump trimmed with `extract-xiso -r` to a 3,112,328,144-byte xiso, copied by FTP to `E:\xcat\isos\` (~11.7 MB/s), installed: **21 files in 211,952 ms (~14,340 KB/s, ~14.7 MB/s)**. Read-back verify: all 21 files match. **The installed game launches and plays from the dashboard.** M2 exit test met. |
| 2026-10-14 | M2 (step 2) | `main` after #25 (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/` (XBE + `samples/`) copied by FTP. Bundled `small.iso` installed to `E:\Games\small`: 9 files in 19 ms (10,141 KB/s; tiny image, so not a throughput figure). Read-back verify: all 9 files match. |
| 2026-10-14 | M2 (step 1) | `feat/onbox-extract` (local Docker build) | xemu v0.8.136, kernel 1.0.4627.1 | ✅ Pass | Bundled `small.iso` copied to `E:\xcat\isos\`, installed to `E:\Games\small`: 9 files, 3 folders, 192 KB in 62 ms (3,107 KB/s, emulated disk). Read-back verify: all 9 files match the image. "Done: 1 of 1 image(s) installed and verified." |
| 2026-10-13 | M1 | `feat/extractor` | Host: Windows (cl, clang-cl, clang), Ubuntu 24.04 (gcc, clang) | ✅ Pass | `xcat-cli extract` output matches each fixture's manifest (all layouts, incl. >4 GiB on Linux). Direct check: `diff -r` against `extract-xiso -x` output is identical for `small` (9 files) and `deep` (311 files, 17 dirs). Parser fuzzed (#21). |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | Hardware: main console (see bench) | ✅ Pass | `app/bin/default.xbe` copied with FileZilla (FTP) and launched from the dashboard. Same three lines as in xemu. |
| 2026-10-13 | M0 | `e4c26ab` (local Docker build) | xemu v0.8.136 (Windows 11) | ✅ Pass | `app/xiso-cartographer.iso` via Load Disc. Shows `xiso-cartographer 0.0.0`, `libxcat version number: 0x000000`, `Hello from nxdk. Toolchain OK.` |

## M2: on-box install from the hard disk

**Exit test:** an image already on the HDD installs to `E:\Games\<title>` in xemu and on hardware, and every installed file matches the image (checked on the console itself; see [build-xbox.md](build-xbox.md#what-the-app-does)).

| Step | Image | Where | How |
|---|---|---|---|
| 1 | Bundled `small.iso` (synthetic) | xemu | Load Disc → `app/xiso-cartographer.iso`; the app copies the sample to `E:\xcat\isos\` and installs it |
| 2 | Bundled `small.iso` | Hardware | FTP `app/bin/` (XBE + `samples/`) to `E:\Apps\xiso-cartographer\` and launch |
| 3 | One of your own games as a trimmed xiso under 4 GiB | Hardware | FTP it to `E:\xcat\isos\`, launch; then launch the installed game from the dashboard |

A run passes when the screen ends with "Done: N of N image(s) installed and verified." Record the summary lines (files, KB/s) in the results log above.

## M3: network download test

**Exit test** ([HANDOFF.md §5](HANDOFF.md)): a 1 GB download over the LAN whose CRC-32 matches, with MB/s recorded, that survives a pulled cable. `app/net-test` downloads one file with the core HTTP client (the same code `xcat-cli get` uses) over the console's own network stack (lwIP, `platform/nxdk/src/nx_net.c`), and reports progress, every retry, the average MB/s and the CRC-32.

**On the PC:**

1. Make the test file (pseudo-random test data, never a game): `python3 tools/testserver/make_test_file.py --out C:\xcat-net --size 1G`. It prints `url=`, `size=` and `crc32=` lines.
2. Serve it to the LAN: `python3 tools/testserver/rangeserver.py --root C:\xcat-net --bind 0.0.0.0`. Allow Python through the Windows firewall (private networks) when asked. Optional check from the PC: `xcat-cli get http://127.0.0.1:8080/xcat-test-1073741824.bin <file>` prints the same CRC-32.
3. Write `net-test.txt` from the printed lines, with `<pc-ip>` replaced by the PC's LAN address (`ipconfig`):

   ```
   url=http://192.168.1.10:8080/xcat-test-1073741824.bin
   size=1073741824
   crc32=<printed by make_test_file.py>
   ```

   Optional lines: `save=yes` also writes the file to `E:\xcat\net-test.bin` (network and disk together, like a real install; delete the file afterwards), and `attempts=N` (default 40, about half an hour of outage) sets how many consecutive failures end the test.

**On the console:**

| Step | Where | How |
|---|---|---|
| 1 | xemu (optional) | Enable networking (**Settings → Network**, NAT). The PC is then usually `10.0.2.2`. Put `net-test.txt` on the E: partition of the HDD image, then **Load Disc →** `app/net-test/xcat-net-test.iso`. A small `--size 100M` file is enough here. |
| 2 | Hardware | FTP `net-test.txt` to `E:\xcat\` and `app/net-test/bin/default.xbe` to e.g. `E:\Apps\xcat-net-test\`. Launch it. Let it run to the end once without touching anything. |
| 3 | Hardware | Launch it again. After about 30 s, **pull the network cable** for 30–60 s, then plug it back in. The screen should show retries ("The server stopped responding" or "network connection failed"), then progress resuming from where it stopped, never from 0. |
| 4 | Hardware (optional) | Once more with `save=yes`, for the speed with the disk writes included. |

The cable pull tests a lost connection within one run. A power cut or reboot can't be resumed by this test: it keeps its progress in memory only, so the next launch starts from 0. Resuming an install after a power cut is M4's exit test (a journal on disk).

A run passes when the screen ends with "CRC-32 … matches, PASS". Each run is appended to `E:\xcat\net-test.log` (it starts over past 1 MiB); fetch it over FTP and record in the results log: the average MB/s, the retries and the longest pause (step 3), and the disk-write line (step 4). The MB/s figure also sizes the install buffers ([ARCHITECTURE.md §6](ARCHITECTURE.md)).

## M4: streamed install over HTTP

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): an over-4 GB fixture installs over the LAN, and resumes after a power cut. The main app (`app/bin/default.xbe`) installs every entry of `E:\xcat\map.txt` while it streams in, with a journal in the install folder ([journal.md](formats/journal.md)), then reads every file back and compares its CRC-32.

**On the PC:**

1. Make the fixtures, including the 4.3 GB `large` image. It's sparse, so it takes almost no disk space, and it holds test data only, never a game: `python3 tools/fixtures/fixtures.py --out C:\xcat-fixtures --large`.
2. Serve them to the LAN: `python3 tools/testserver/rangeserver.py --root C:\xcat-fixtures --bind 0.0.0.0`. Optional check from the PC: `xcat-cli install http://127.0.0.1:8080/small.iso <some folder>` ends with "verified".
3. Write `map.txt` with the PC's LAN address:

   ```
   # xcat-map v1
   00000001 = http://192.168.1.10:8080/large.iso
   ```

   (Any 8 hex digits work as the key until the catalog exists. The install folder is named after the file: `E:\Games\large`.)

**On the console:**

| Step | Where | How |
|---|---|---|
| 1 | Hardware | FTP `map.txt` to `E:\xcat\` and `app/bin/` to e.g. `E:\Apps\xiso-cartographer\`. Empty `E:\xcat\isos\` first, or the M2 test image installs again too. About 4.4 GB must be free on E:. |
| 2 | Hardware | Launch it. The screen shows "writing files" with MiB, file count and MB/s every 5 s. At roughly a third or half way, **switch the console off at the power button**. |
| 3 | Hardware | Switch it on and launch the app again. The progress must pick up near where it stopped (at most 64 MiB earlier), not at 0. It ends with "OK: … verified (resumed)", the line "1 file(s) continued from a checkpoint (… MiB were already there)", and "Done: 1 of 1". |
| 4 | Hardware (optional) | Launch once more: the game goes to `E:\Games\large (2)`, because the first one is finished ("already installed there"). Delete that folder afterwards. |

A pass is "OK: … verified (resumed)" in step 3. Send `E:\xcat\install.log`: it has every run, the MB/s, and for any retry the time the last connection's lookup and handshake took (that's to pin down the one-off 15 s first-request delay from M3). Delete `E:\Games\large` afterwards; it's test data.

## M5: TLS speed on the console

**Why now:** HTTPS speed on the console is the go/no-go for the Xbox-only design (target ≥ 1 MB/s, [HANDOFF.md §4](HANDOFF.md#4-strategies): "prove the risk early"). This run measures it with `app/net-test` before the installer uses TLS. The full M5 exit test (installing the archive.org test item over HTTPS, and rejecting a bad certificate and a wrong clock) follows once NTP and the installer's HTTPS are in.

For an `https` URL the test first prints:
- how fast the CPU runs the two cipher families with no network involved (the most TLS can do on this CPU);
- the console's clock;
- the randomness it gathered (timer samples, bits credited, a health figure out of 256);
- the time it checks certificates against ([sntp.md](formats/sntp.md)): from NTP (`pool.ntp.org` unless `net-test.txt` says `ntp=<server>`; `ntp=off` skips it), with the server's stratum, the round trip, the number of requests and how far the console's clock is off; or, if NTP failed (and why), from the console's clock;
- which roots it trusts.

It then prints how long connecting took and which TLS version and cipher suite the server chose, then the download as before.

**Before you start:** since NTP is in, the console's clock only matters when NTP fails or is off: then certificates are checked against it, and a clock that was reset (earlier than the XBE's build) is refused with a message saying so.

| Step | Where | How |
|---|---|---|
| 1 | Hardware, internet | `E:\xcat\net-test.txt` with the archive.org test item's download URL (`url=https://archive.org/download/<item>/<file>`) and `limit=268435456`, so only its first 256 MiB are fetched. FTP `app/net-test/bin/default.xbe` from the CI artifact to e.g. `E:\Apps\xcat-net-test\` and launch it. Make sure `E:\Apps\xcat\cacert.pem` doesn't exist, so the built-in roots are used. |
| 2 | Hardware, LAN (optional, recommended) | HTTPS from the PC, which shows the TLS cost without the internet link in the way. On the PC (Git Bash): `sh tools/testserver/make_lan_cert.sh <pc-ip> C:/xcat-lan`, `python3 tools/testserver/make_test_file.py --out C:\xcat-net --size 256M`, then `python3 tools/testserver/rangeserver.py --root C:\xcat-net --bind 0.0.0.0 --tls-cert C:\xcat-lan\lan-cert.pem --tls-key C:\xcat-lan\lan-key.pem`. FTP `C:\xcat-lan\lan-cert.pem` to the console as `E:\Apps\xcat\cacert.pem`. Use `url=https://<pc-ip>:8080/xcat-test-268435456.bin` with the printed `size=` and `crc32=`. The run should end "matches, PASS". Optionally run it again with the server started with `--tls-ciphers ECDHE+AESGCM` (AES-GCM instead of ChaCha20). |
| 3 | Hardware (optional preview of the exit test) | With step 2's `cacert.pem` still in place, run step 1's archive.org URL: it must fail with "The server's certificate isn't trusted". Then **delete `E:\Apps\xcat\cacert.pem`**, and check the clock both ways (ADR 0012 §5): (a) set the dashboard clock a few years **ahead**, add `ntp=off` to `net-test.txt` and run step 1: it must fail with "isn't valid at the current time (is the clock right?)"; (b) remove `ntp=off` and run it again with the clock still wrong: it must say "Time from NTP", report the clock as years off, and download normally; (c) set the clock to **2001**, add `ntp=192.0.2.1` (an address nothing answers on) and run it: after three NTP requests it must stop with "Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)". Set the clock back and remove the `ntp=` line afterwards. |

Send `E:\xcat\net-test.log`. Record in the results log: the cipher speeds, the randomness line, the suite each server chose, the connect time and the MB/s. Since the TCP tuning (`platform/nxdk/lwip/lwipopts.h`), each run also ends with a `TCP:` line: the receive window, frames and segments in, and what was dropped or bad, or ran out of memory. Record it too; drops and memory shortages point at lost packets on the console's side. During the download, a `TCP now:` line (at 10 s, then once a minute) shows what the connection agreed: window scaling on or off, with both scale factors, SACK, our receive window and what we advertised, the server's window and congestion window. If scaling shows `OFF`, the receive window is stuck at 64 KB. Each `TCP now:` line comes with a `Reading:` line for the time since the previous one (the run's summary has one for the whole download): the share of the time the app spent waiting for data, how many reads found our window closed, and how much data was waiting unread, on average and at most. If the app waits most of the time, the data arrives too slowly (the server or the path). If it rarely waits and the window is often closed, the console is too slow. `Reading:` also counts the reads that found out-of-order data queued, a sign of lost segments. The first lines give the link's speed and duplex (`HALF` duplex loses frames to collisions), and the `TCP:` line the frames the driver passed to lwIP and how many lwIP refused because its input mailbox was full, and the most of the driver's receive buffers lwIP held at once (when all are held, the network chip drops frames). The `connected in` line gives the TCP handshake time, about one round trip to the server the data comes from: one receive window per round trip is the most a connection can move. archive.org's suite is worth noting in any case (ADR 0005 asks to watch for changes).

## M5: exit test (install over HTTPS)

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): the archive.org test item installs on hardware over HTTPS, at 1 MB/s or more; a bad certificate and a wrong clock are both rejected. The main app (`app/bin/default.xbe` from the CI artifact) does it: `https://` map entries, NTP for the time, the built-in roots, and the whole-image MD5 and SHA-1 ([journal.md §7](formats/journal.md#7-verification)). [build-xbox.md](build-xbox.md#https) explains the HTTPS setup it logs.

**Before you start:** make sure `E:\Apps\xcat\cacert.pem` and `E:\Apps\xcat\ntp.txt` don't exist, `E:\xcat\isos\` is empty, and about 3.6 GB are free on E:. Write `map.txt` with the test item's download URL (the same one as the TLS speed test) and FTP it to `E:\xcat\`:

```
# xcat-map v1
00000001 = https://archive.org/download/<item>/<file>
```

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | Hardware, internet | Launch the app. It sets up TLS ("Randomness", "Time from NTP", "Trusting the 121 built-in roots"), then installs the whole image. Optionally switch the console off once mid-way and launch again: it must resume (over HTTPS, from the journal). | "OK: … verified", the line "wrote … MiB in … s (X MB/s)" with X ≥ 1.0, a "TLS 1.2, …" line, and the image's MD5 and SHA-1. They must equal the values archive.org lists for that file (the item's `https://archive.org/metadata/<item>`, entry `files`, fields `md5` and `sha1`). Launch the game from the dashboard. |
| 2 | Hardware | **Bad certificate:** copy the LAN test certificate (`C:\xcat-lan\lan-cert.pem` from the TLS speed test, step 2) to `E:\Apps\xcat\cacert.pem` and launch the app. | "NOT INSTALLED: The server's certificate isn't trusted (unknown issuer or wrong name)". Delete `cacert.pem` afterwards. |
| 3 | Hardware | **Wrong clock, NTP off:** put `off` in `E:\Apps\xcat\ntp.txt`, set the dashboard clock a few years **ahead**, launch. | "NOT INSTALLED: The server's certificate isn't valid at the current time (is the clock right?)". |
| 4 | Hardware | **Clock reset, NTP unreachable:** set the clock to **2001**, put `192.0.2.1` (an address nothing answers on) in `ntp.txt`, launch. | After three NTP requests: "HTTPS isn't available: Couldn't get the correct time (NTP server unreachable and the console's clock is wrong)". Then delete `ntp.txt` and set the clock back. |

Steps 2–4 find the game from step 1 already installed, so each would go to a numbered folder; they stop before creating one. Send `E:\xcat\install.log`. Record the MB/s, the hashes and whether they matched, the time source, the suite, and the three rejections in the results log (without the item's name, [ADR 0006](adr/0006-no-links-no-bypass.md)).

## M5b: exit test (install from a zip)

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): a full-disc Deflate zip installs over HTTPS on hardware, every byte verified by the member's CRC-32, and an interrupted install resumes from a checkpoint. The main app (`app/bin/default.xbe` from the CI artifact) does it: map entries ending in `.zip` ([build-xbox.md](build-xbox.md#zips)), Deflate decompressed as it streams in, with the spool, the second pass and the saved states of [stream-install.md](formats/stream-install.md).

**Before you start:** a full-disc Deflate zip you have access to on archive.org (like the one `xcat-cli zip` showed: Zip64, Deflate, one `.iso`), with its login in `E:\Apps\xcat\credentials` if it needs one. Free space on E: of at least the game's files plus a few GB: the spool uses what's free above 256 MiB, and more space means less read again. `xcat-cli layout` on the PC (if you have the ISO) tells you the game's size. Make sure `E:\Apps\xcat\cacert.pem` and `E:\Apps\xcat\ntp.txt` don't exist and `E:\xcat\isos\` is empty. Write `map.txt` with the zip's download URL (the `.zip` itself, not a file inside it) and FTP it to `E:\xcat\`:

```
# xcat-map v1
00000001 = https://archive.org/download/<item>/<file>.zip
```

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | Hardware, LAN (optional rehearsal) | On the PC: `python3 tools/fixtures/fixtures.py --out C:\xcat-fixtures --only late`, `python3 tools/fixtures/zipgen.py --fixtures C:\xcat-fixtures late`, then serve the folder with `rangeserver.py` as for M4 and put `00000002 = http://<pc-ip>:8080/late-deflate.zip` in a test `map.txt`. `late` has its directory tables after its files, so every file goes through the spool on FATX. | "OK: 9 files … verified", "from the zip: late.iso, Deflate" and "1 pass, 0 MiB read again, 8 file(s) from a … spool". No `.xcat-spool-*` or `.xcat-state-*` files are left in `E:\Games\late`. Delete that folder afterwards. |
| 2 | Hardware, internet | Launch the app with the archive.org `map.txt`. It reads the zip's directory ("reading the directory tables"), then "writing files" with MiB of the image, file count and MB/s every 5 s. Once it's well past the first few hundred MiB, **switch the console off at the power button**. | — |
| 3 | Hardware, internet | Switch it on and launch the app again. The progress must pick up at most 64 MiB before where it stopped, not at 0. Let it finish. | "OK: … verified (resumed)", "from the zip: <file>.iso, Deflate, …", and "continued N MiB into the image" with N > 0. A damaged byte anywhere would fail the install with "The image in the zip failed its CRC-32 check", so "verified" means every byte matched. Launch the game from the dashboard. |
| 4 | Hardware | **A link into the zip:** change the entry to `https://archive.org/download/<item>/<file>.zip/<file>.iso` and launch. | When the map loads: "error: URL points to a file inside a zip, which can't be resumed; use the URL of the .zip itself (ADR 0009)", then "Map: 0 entries". Put the `.zip` URL back afterwards. |

Send `E:\xcat\install.log`. Record in the results log (without the item's name, [ADR 0006](adr/0006-no-links-no-bypass.md)): the zip's size and compression, the MB/s of writing and of the read-back, the number of passes, the MiB read again and the spool's largest size, where the resume continued, the free space you started with, and whether the game launches. If a second pass ran, also note how long pass 1 took; that's the cost ADR 0009 amendment 1 asks to show after pass 1.

## M6: exit test (archive.org source)

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): an automatic map from an archive.org item, installed files verified, and a clear error for a removed item. The main app (`app/bin/default.xbe` from the CI artifact) does it: `@archiveorg` ([archiveorg.md](formats/archiveorg.md), [ADR 0013](adr/0013-archiveorg-source.md)), the catalog list ([build-xbox.md](build-xbox.md#what-the-app-does)), and the listing's size and hashes checked at install.

**Before you start:** your login in `E:\Apps\xcat\credentials` (the item's files need it; the listing doesn't). Make sure `E:\xcat\isos\` is empty, so the list only holds the item's games. Write `map.txt` with your item and an identifier that doesn't exist, and FTP it to `E:\xcat\`:

```
# xcat-map v1
@archiveorg <your item>
@archiveorg xcat-no-such-item-test
```

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | PC (optional) | `xcat-cli map map.txt` | One line per game of the item, with its size and `md5, sha1, crc32`; an error on line 3: "archive.org item xcat-no-such-item-test doesn't exist or was removed"; "N entries from 1 file, 1 error". |
| 2 | Hardware, internet | Launch the app. | The log has the same error for line 3 and "Map: N entries", then "Catalog: N game(s)". **Nothing installs.** Press A: the list shows the item's games, each with its size in MiB, and "1 of N". |
| 3 | Hardware | Move with the D-pad (up/down, wrap at either end), page with left/right. | The highlight (`>`) and "k of N" follow; pages move by 18. |
| 4 | Hardware, internet | Highlight a small game and press A. | "Picked: <game>", "archive.org says: <bytes> bytes", the usual install lines, "OK: … verified", "from the zip: …", a "title ID XXXXXXXX, "<name>" (from default.xbe)" line, then "Installed and verified." Press A: the game's line is marked `ok`. |
| 5 | Hardware | Back to the dashboard (Back), launch the game. | It runs. `E:\Games\<game>\.xcat-installed` has `title_id=` and `title=` lines ([journal.md §7.1](formats/journal.md#71-the-marker)). |

A wrong size or hash, and a taken-down item, can't be produced on demand on archive.org, so they're covered by the unit tests (`test_install`, `test_map`, `test_archiveorg`). Send `E:\xcat\install.log`. Record in the results log (without the item's or game's name, [ADR 0006](adr/0006-no-links-no-bypass.md)): how many games the item listed, how long loading the map took, the game's size, MB/s and passes, the title ID line, and whether it launches.

## M7: exit test (catalog pipeline)

**Exit test** ([HANDOFF.md §4.1](HANDOFF.md#41-roadmap)): a deterministic rebuild, schema tests, and licences reviewed and credited ([ADR 0014](adr/0014-catalog-sources-and-format.md)). Steps 1–3 run on a PC and in CI. Step 4 checks the console's use of the catalog ([build-xbox.md](build-xbox.md#the-catalog-m7)).

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | PC with the cache (`%USERPROFILE%\xcat-catalog-cache`) | `python3 -I tools/catalog/build.py --cache <cache> --out build/catalog` on two machines or OSes (e.g. Windows and `docker run python:3.12-slim`) | Both print the same `catalog.bin sha256` and "matches tools/catalog/inputs.lock" |
| 2 | CI | `test_catalog`, `fuzz_catalog` (seeds every build, 60 s of fuzzing in the `fuzz` job), `catalog_cli_*`, `catalog_pipeline` | All pass. The C reader checks every rule of [catalog.md §7](formats/catalog.md#7-reading), and `catalog_pipeline` opens the pipeline's output with it |
| 3 | Review | [`tools/catalog/SOURCES.md`](../tools/catalog/SOURCES.md), `CREDITS.txt` from step 1 | Each source has what is taken, its licence or the lack of one, and a check date; `CREDITS.txt` credits Wikidata (CC0) and Redump, with the snapshot |
| 4 | Hardware | Put `build/catalog/catalog.bin` in `E:\Apps\xcat\`, keep the M6 `map.txt` (`@archiveorg <your item>`), launch the app, press A. Pick a small game. | The log says "Catalog: N games, M releases (wikidata …)". Games known to the catalog show its title and a `[XXXXXXXX@VARIANT]` tag, with the Redump name under the list; others say `not in catalog`. After the install: "matches the catalog: …" under the title ID line. Then rename `catalog.bin` and relaunch: the list shows file names again, and the log says "No catalog" |

Record in the results log: how many of the item's games the catalog named, and the "matches the catalog" line (without the game's name, [ADR 0006](adr/0006-no-links-no-bypass.md)).

## M8: UI test (rendering)

The UI test XBE (`app/ui-test`, [build-xbox.md](build-xbox.md)) draws through the same SDL backend, text helpers and widgets the M8 screens will use ([ADR 0015](adr/0015-ui-sdl2-own-widgets.md), [ui-style.md](ui-style.md#widgets)). Run it before the screens are built, to check the fonts, the widgets with a real controller, and the drawing speed.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu | Load `app/ui-test/xcat-ui-test.iso` | One screen in the UI's look ([ui-style.md](ui-style.md)), no debug text: the green panorama behind everything; a "UI TEST" tab in the wide title font (Michroma); GAMES (highlighted), INSTALLED, SETTINGS in the squarish label font (Chakra Petch), none cut; a DOWNLOAD panel whose bar fills over 20 seconds with its percentage on it, then starts again; a list of "Test game" lines; a detail panel with "Game 1", "Café, naïve, Ærøskøbing" with the accents, a Greek and Cyrillic line, two boxes after "Boxes:", a line cut with "…", and a wrapped paragraph ending in "…"; a footer. The panels have cut corners (top left, bottom right) and the background shows through them a little. The screen stays steady (no black frames) |
| 2 | xemu | Watch for a minute and a half | The background pans to the right at a steady pace, slows to a stop in about a second at the end, then pans back, without jumps or a long crawl at the ends. The footer's first line starts "Change" with up to about 20 frames a second ("N/s"), then a frame's time as drawing + finish + flip in ms: the screen's drawing, SDL's work left when it's presented, and writing the frame from the cache to memory for the screen. Then the drawing's parts: "copy" (images), "blend" (translucent fills), "sdl" (text and borders), "size" (measuring text), "text" (rendering text not yet cached) |
| 3 | xemu | D-pad up and down, then hold Down; LT and RT | The highlighted line moves, the list scrolls inside its panel, nothing is drawn outside it, and the detail panel names the line. Held, Down repeats after under half a second, about 12 lines a second, and stops at the last line; LT and RT move a page. The scroll bar at the list's right edge follows. "text" stays near 0 once every line has been seen |
| 4 | xemu | White, then White again | "Every" at the TV's refresh, about 60 frames a second, then back to "Change". Drawing + finish + flip stays under 16 ms, or the rate halves |
| 5 | xemu | A; Left and Right; A. Then A and B | A dialog titled INSTALL over the darkened screen, which stands still while it's open, with INSTALL, DETAILS and CANCEL; Left and Right move the highlight and stop at the ends; A closes it and the footer says "Dialog: <choice> for Test game N". B gives CANCEL |
| 6 | xemu | Y; type a word with the D-pad and A, with LT for capitals, RT for symbols, Y for a space, X to delete; hold a direction; Start | The keyboard over the darkened screen: SEARCH, the field with a caret, 10 × 5 keys. The D-pad wraps around the edges and repeats when held; on the bottom row it goes key by key (SHIFT, #+=, SPACE, DELETE, DONE). SHIFT turns accent green while capitals are on, #+= becomes ABC on the symbols. A long text shows its end. Start closes it; the footer says `Search: done "<text>"`. Y again opens it with the text kept; B gives "cancelled" |
| 7 | xemu | X; type a few characters; Start or B | PASSWORD, with one dot per character, never the characters. The footer gives the number of characters and "(wiped)": the test clears the buffer |
| 8 | Hardware | Copy all of `app/ui-test/bin/` (with `fonts\` and `ui\`) to the console, launch it, repeat 1–7 on the TV with a controller | The same; the panels, the dialog and the keyboard sit fully inside the TV's picture (title-safe area). With White, about 60 frames a second on the list and with the keyboard open. Back returns to the dashboard |

Record the footer's first line (all of it) on change and with White, on the list and with the keyboard open, on xemu and on hardware. The hardware figures decide: a frame must fit in one refresh (16.7 ms).

The screens draw a frame only when something on them changed, like the "On change" mode: each frame is drawn whole by the CPU, which an install needs for itself.

## M8: library and install screens

The main app's first screens ([build-xbox.md](build-xbox.md#what-the-app-does), [ui-style.md](ui-style.md#screens)). Copy all of `app/bin/` (with `fonts\`, `ui\` and `samples\`) over the app's folder on the console. Keep the M6/M7 `map.txt` and `catalog.bin`.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Launch the app | The startup lines on the debug console (credentials, catalog, map), then the LIBRARY screen: GAMES highlighted, the games in the list, the first one's details beside it (with the catalog: year, region, size, developer, the Redump name and key, "From archive.org…"; without: "Not in the catalog" or "No catalog loaded"), SUMMARY with the number listed, and "Pick a game and press A…" in the footer. The background pans |
| 2 | Hardware | D-pad down and up, hold Down, LT and RT; then Left, Down, Right; then Left, Up, Right | The list moves and pages as in ui-test, and the details follow. Left moves to the sections (bright border) (INSTALLED: see [M8: installed games](#m8-installed-games)); back on GAMES the list is where it was |
| 3 | Hardware | Back, then B. Back, then A | LEAVE / STAY over the darkened screen; B stays. LEAVE returns to the dashboard |
| 4 | Hardware, internet | Highlight a small game, A, then B (CANCEL); A, then A (INSTALL) | The first time nothing installs. Then the INSTALL screen: CONNECTING with the network's steps ("Starting the network…", HTTPS's steps, "Opening the file on the server…"), then CHECKING THE IMAGE, WRITING FILES with the bar, the sizes, the file and the speed and time left, then VERIFYING. Note the speed: compare it with the same game's M6/M7 runs (the screen's drawing costs the install some CPU) |
| 5 | Hardware, internet | During WRITING FILES: B, then A (CONTINUE); later B, Left, A (STOP) | CONTINUE carries on with nothing lost. STOP ends with NOT INSTALLED: "Stopped. What's written is kept…" with RETRY and BACK. RETRY starts it again and it carries on from where it stopped (the log says it resumed, or continued N MiB in). Stop and RETRY once more, on a zip past its first 400 MB (where it slows to about 2 MB/s): the dialog answers each press at once. Let it finish |
| 6 | Hardware | At the end | INSTALLED: "In E:\Games\<title>, every file read back and checked." OK returns to the library: the game says INSTALLED, SUMMARY counts it, the footer "<game>: installed". The game launches from the dashboard |
| 7 | Hardware | Pull the network cable during WRITING FILES for a minute, then plug it back in | The status line says what failed and when it tries again ("… Trying again in N s (2 of 40)."); B still works between retries. With the cable back, it carries on and the status line clears |
| 8 | Hardware | A game that can't install, e.g. one bigger than the free space, or a map entry whose file was removed | NOT INSTALLED with the reason and its details ("Not enough free space on the drive. It needs 4.38 GB; 1.20 GB is free.", or "The file wasn't found on the server. HTTP 404."). BACK: the game says FAILED with that message |

Record: whether the steps passed, the install's MB/s against its earlier runs, and anything cut off or hard to read on the TV.

## M8: installed games

The library finds what's already in `E:\Games` ([ui-style.md](ui-style.md#library), `xcat/installed.h`). Copy `app/bin/` as above; keep the games installed in earlier tests.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Launch the app | Games installed in earlier runs say INSTALLED with "In E:\Games\<folder>", without being installed again, and SUMMARY counts them. The log has "E:\Games: N folder(s) from this app; of the games listed, N installed and N unfinished" |
| 2 | Hardware | Left, Down, Right; move in the list | INSTALLED lists the folders this app installed, by name, with the size, the number of files, INSTALLED, the folder, the title and title ID, "Checked: every file's CRC-32" (and the published hash for archive.org images), and the server's name or the file's path. Folders other tools made aren't there. A does nothing; Left, Up, Right goes back to GAMES where it was |
| 3 | Hardware, internet | Install a game and STOP part way (B, STOP, BACK); leave for the dashboard and launch the app again | The game says UNFINISHED: "Started in an earlier visit, in E:\Games\<folder>", and SUMMARY counts 1 unfinished. INSTALLED lists the folder as UNFINISHED |
| 4 | Hardware, internet | Install that game again and let it finish | It carries on from where it stopped. Back in the library it says INSTALLED, and INSTALLED shows its folder as INSTALLED at once, without a restart |

## M8: search

Y searches GAMES ([ui-style.md](ui-style.md#library)). Copy `app/bin/` as above, with a map listing several games.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Y; type part of a game's name; Start | The keyboard opens over the library, labelled SEARCH. After Start, GAMES lists only the games whose name, year, region, maker or origin has every word typed; the SEARCH panel by the title shows the words and "N of M"; the highlighted game stays highlighted if it matches |
| 2 | xemu, then hardware | A on a game found | "Install <that game>?" names the game highlighted in the searched list; CANCEL |
| 3 | Hardware | B | Every game is listed again, the SEARCH panel goes, and the highlight stays on the same game |
| 4 | Hardware | Y; type a word no game has; Start; then Y, DELETE it all, Start | "No game matches" with "0 of M", and A does nothing. Searching for nothing lists every game. Y then B in the keyboard leaves the list as it was |
| 5 | Hardware | Left to INSTALLED; Y; type; Start | The search applies to GAMES, which is shown; INSTALLED keeps its list |

## M8: on-box login

Logging in to archive.org on the Xbox ([login.md](formats/login.md), [ADR 0010](adr/0010-on-box-login-required-for-beta.md)). Copy `app/bin/` as above. If `E:\Apps\xcat\credentials` was written by hand, keep a copy of it first (FTP): logging out removes its `[archive]` section.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Left, Down three times, Right, Down, A (since M8: authentication, archive.org is the first server on SETTINGS' Authentication page) | Authentication lists archive.org. The details say LOGGED IN if the credentials file has an `[archive]` section, otherwise NOT LOGGED IN, and that the password is never stored. The footer says "A log in" or "A log out" |
| 2 | Hardware | If LOGGED IN: A, LOG OUT | "LOGGED OUT", then NOT LOGGED IN. Over FTP, `credentials` has no `[archive]` section, and every other section and comment is unchanged. There's no `credentials.tmp` |
| 3 | xemu, then hardware | A; type part of an email; Start; type a few letters; B | The keyboards say ARCHIVE.ORG EMAIL, then ARCHIVE.ORG PASSWORD, which shows only dots. B closes it, and nothing logs in |
| 4 | Hardware, internet | A; the account's email (kept from step 3, finish it); Start; a wrong password; Start | "Logging in to archive.org..." in the footer, then a message: "The password is wrong. Try again." Still NOT LOGGED IN |
| 5 | Hardware, internet | A; Start (the email is kept); the right password; Start | A LOGGED IN message, then LOGGED IN in SETTINGS. Over FTP, `credentials` has the `[archive]` section of login.md §4 (a comment line, `hosts`, `cookie`) and its other sections as before. **Neither the password nor the email appears** in `credentials` or `E:\xcat\install.log`; the log says "Logging in to archive.org" and "Logged in to archive.org: [archive] written to ..." |
| 6 | Hardware, internet | Install an item that needs the login | It installs (M8 exit test: "a login-required item installs") |
| 7 | Hardware | Leave for the dashboard and launch the app again | SETTINGS still says LOGGED IN |

## M8: log in again

An expired or revoked session asks for a new login, then the install carries on ([login.md §7](formats/login.md#7-logging-in-again), ADR 0010 decision 4). archive.org sessions last a long time, so the test spoils the saved session instead of waiting: to archive.org, a wrong signature looks the same as an expired or revoked one. Start logged in (M8: on-box login).

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | Hardware, internet | Install an item that needs the login, and STOP it part way (B, STOP, BACK). Leave for the dashboard | The folder is UNFINISHED |
| 2 | Hardware | Over FTP, keep a copy of `E:\Apps\xcat\credentials`, then change a few characters in the middle of the `logged-in-sig=` value (keep the same kinds of characters: letters for letters, digits for digits) | — |
| 3 | Hardware, internet | Launch the app; install the same item | NOT INSTALLED: "Access denied by the server", "The login for [archive] is missing or expired", "Log in again, and the install carries on where it stopped", with **LOG IN** and BACK. The log has the same, and the HTTP status. Nothing was retried. (If the dialog says something else, such as RETRY with another error, write down the log lines: archive.org answered a bad session differently from 401/403) |
| 4 | Hardware | LOG IN; B on the email keyboard | Back in the library, nothing installs. Installing the item again gives the same LOG IN dialog |
| 5 | Hardware, internet | LOG IN; the email; Start; the password; Start | "Logging in to archive.org...", then the install screen for the same item at once, carrying on from where step 1 stopped (the log says files were kept or continued from a checkpoint). It installs and verifies. `credentials` has the new `[archive]` section; the password and email aren't in it or in the log |

## M8: install location

Choosing where games install in SETTINGS ([settings.md](formats/settings.md), [ADR 0016](adr/0016-settings-file-and-install-location.md)). Copy `app/bin/` as above. Steps 4–6 need a drive with an F: (or G:) partition; on a stock drive, step 3 shows that F: isn't offered.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Left, Down three times, Right | SETTINGS lists Install location and archive.org. Install location's details show `E:\Games` (or the folder chosen before) |
| 2 | xemu, then hardware | A | The list shows `E:\Games` (and `F:\Games`, `G:\Games` for the partitions the drive has), then Other folder. The current folder is highlighted, its details say CURRENT, and each folder shows its free space. B goes back |
| 3 | xemu, then hardware | A, Down to Other folder, A; DELETE it all, type `qq`, Start | NOT CHANGED: "qq" must start with E:\, F:\ or G:\. Nothing is written: `E:\Apps\xcat\settings` is as it was (or still not there) |
| 4 | Hardware, F: partition | A on `F:\Games` | "Games install to F:\Games from now on". Over FTP, `F:\Games` exists and `E:\Apps\xcat\settings` has `install_dir = F:\Games`. INSTALLED lists what's in `F:\Games` (likely nothing), and GAMES no longer marks the games in `E:\Games` |
| 5 | Hardware, F: partition | Install a game | "Install <name> to F:\Games?"; it installs into `F:\Games\<title>` and verifies. The free space the install checks is F:'s |
| 6 | Hardware, F: partition | Leave for the dashboard; launch the app again | Installs still go to `F:\Games`, and the game from step 5 is INSTALLED. Choosing `E:\Games` again marks the games there as before |
| 7 | Hardware | Over FTP, change `install_dir` in `settings` to `C:\Games`; launch the app | A SETTINGS message: line 3 must start with E:\, F:\ or G:\; games install to E:\Games. Choosing a folder then says the file has a problem and doesn't change it. Delete the file, and it works again |

## M8: authentication

SETTINGS' Authentication page: the servers the map uses and their logins ([ui-style.md](ui-style.md#library), [credentials.md §8](formats/credentials.md#8-handling-secrets)). Copy `app/bin/` as above. Use a map with entries on archive.org and on at least one other server, for example a PC on the LAN (`http://192.168.x.y:port/...`). Keep a copy of `E:\Apps\xcat\credentials` (FTP), and add a section for the LAN server by hand if it has none, as in credentials.md §1 (any header will do for this test).

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Left, Down three times, Right, Down | SETTINGS lists Install location and Authentication. Authentication's details say archive.org LOGGED IN or NOT LOGGED IN |
| 2 | Hardware | A | archive.org first, then the map's other servers, each once, in the map's order; a plain-http one is shown as `http://host:port`. archive.org's details say how many games come from it, LOGGED IN or NOT LOGGED IN, and A logs in or out as in "M8: on-box login" |
| 3 | Hardware | Down to the LAN server | Its details say how many games come from it and LOGIN [the section's name] |
| 4 | Hardware | A; CANCEL. A; REMOVE | CANCEL changes nothing. REMOVE says LOGIN REMOVED, and the server now says NO LOGIN. Over FTP, `credentials` has no such section, and every other section (and `[archive]`) is as it was |
| 5 | Hardware | A on a server with NO LOGIN | A message says it has no login and that logins for other servers are added by hand to `E:\Apps\xcat\credentials`. Nothing changes |
| 6 | Hardware | B; then install a game | B goes back to SETTINGS' lines. The install screen's WRITING line says "Writing the game's files to" the install folder chosen in SETTINGS (`F:\Games` if that's the one), not always `E:\Games` |

Afterwards, put the kept `credentials` back over FTP.

## M8: map sources

SETTINGS' Map sources page: adding and removing the map's lines on the console ([map.md §9](formats/map.md#9-editing-on-the-console)). Copy `app/bin/` as above. Keep a copy of `E:\xcat\map.txt` (FTP) first: the steps change it. Have an archive.org item you can use, and a LAN server with a map or an image if you can.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu, then hardware | Left, Down three times, Right; Down, Down | SETTINGS lists Install location, Authentication and Map sources. With each highlighted, the details say what it's for (Authentication explains the page, not archive.org's status) |
| 2 | Hardware | A | "Add source", then each entry and directive of `map.txt` in order (comments aren't listed). A line's details show its kind, line number, the whole line, and any problem the map has on it |
| 3 | xemu, then hardware | A on Add source; B. A; ARCHIVE.ORG; type a few letters; B | B closes the question, and B in the keyboard adds nothing. `map.txt` is unchanged |
| 4 | Hardware, internet | A on Add source; ARCHIVE.ORG; the item's identifier; Start | "Loading the map again...", then SOURCE ADDED with the line and the number of games now listed; the item's games are in GAMES. Over FTP, `map.txt` has `@archiveorg <identifier>` as its last line, and every other line as it was |
| 5 | Hardware | A on Add source; IMAGE; an address; Start; `4D53` (too short); Start | NOT ADDED says the title ID is wrong. A on Add source, IMAGE again: the address is still typed; Start; a full title ID; Start: SOURCE ADDED |
| 6 | Hardware | A on Add source; MAP; `https://` and nothing more, or an address without `http`; Start | NOT ADDED with the reason. Nothing changed |
| 7 | Hardware | A on the line added in step 4; CANCEL. A; REMOVE | CANCEL changes nothing. REMOVE says SOURCE REMOVED; the item's games leave GAMES; the line is gone from `map.txt`, and the other lines and comments are as they were |
| 8 | Hardware | Over FTP, delete `map.txt`; launch the app; add an archive.org item | A new `map.txt`: `# xcat-map v1`, a blank line, and the line |

Afterwards, put the kept `map.txt` back over FTP.

## M8: error messages

Every failed install says what went wrong and what to do, and offers RETRY only when it can help ([ADR 0015](adr/0015-ui-sdl2-own-widgets.md) decision 5, [ui-style.md](ui-style.md#install)). Every code's sentence is checked on the PC (`test_bytes`); these steps check that they reach the screen. Copy `app/bin/` as above. Keep a copy of `E:\xcat\map.txt` (FTP), and add the entries below with Map sources (SETTINGS) or by hand.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | Hardware, LAN | Install a game from a PC on the LAN, and pull the network cable part way | After the retries run out (or B, STOP): NOT INSTALLED with the network error and "Check the network, then try again: what's written is kept", and **RETRY** / BACK. Plug the cable back in, RETRY: it carries on |
| 2 | Hardware | An image entry whose address is right but the file isn't on the server (`00000001 = http://<your PC>/missing.iso`); install it | NOT INSTALLED: "The file wasn't found on the server", "Check the address in the map: the server doesn't have that file", HTTP 404, and **BACK alone** (no RETRY) |
| 3 | Hardware | Put a file that isn't an Xbox image (any small file renamed `test.iso`) in `E:\xcat\isos`; launch; install it | NOT INSTALLED: "Not an Xbox disc image", "Use an Xbox disc image: a trimmed .xiso.iso or a full-disc .iso", and BACK alone. Delete the file afterwards |
| 4 | Hardware | In each case, open `E:\xcat\install.log` | The log has the error, a "What to do:" line with the same sentence, and the details |

Afterwards, put the kept `map.txt` back.

## M8: scripted walkthrough

The M8 exit test's walkthrough ([HANDOFF §4.1](HANDOFF.md#41-roadmap)): the app plays [`tools/ui/walkthrough.txt`](../tools/ui/walkthrough.txt) ([ui-script.md](formats/ui-script.md), [ADR 0015](adr/0015-ui-sdl2-own-widgets.md) decision 6) and checks each screen it leads to. It browses GAMES, types a search on the keyboard, installs the bundled test image (its INSTALL question, the install screen and INSTALLED), lists every game again, opens INSTALLED and each of SETTINGS' pages (Install location, Authentication, Map sources and its ADD SOURCE question), and asks to leave and stays. It changes no setting, login or source. On the PC, `test_ui_script_run` checks that the walkthrough reads and names only screens the app has, and that the library and install screens give those names.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | xemu | `make docker-xbe`; **Load Disc** → `app/xiso-cartographer-walkthrough.iso`; wait about a minute without touching the controller | The screens move by themselves; the footer counts the commands, then says `Script passed: 63 commands, 19 screens checked`. `install.log` has a `Screen:` line for each screen and a `Script:` line for each check |
| 2 | Hardware (optional) | Copy `app/bin/` as usual, plus `tools/ui/walkthrough.txt` as `ui-script.txt` next to `default.xbe`, and `app/bin/samples/small.iso` to `E:\xcat\isos\` if it isn't there; launch | As step 1. Then delete `ui-script.txt` (and the test image's folder in the install folder, if wanted) |

A failed check stops the script there, and the footer says which line, the screen expected and the one shown (`Script FAILED at line …`). The controller works throughout, so the app can be left with Back or LT + RT + Back + Black either way.

## M9: title image

The app's logo as its XBE's title image ([xbe-title-image.md](formats/xbe-title-image.md)). On the PC, `ui_xbe_title_image` checks the encoding and the added section; these steps check that dashboards show it.

| Step | Where | How | Pass |
|---|---|---|---|
| 1 | Hardware | Copy `app/bin/` as usual, over the old `default.xbe`; open the folder in the dashboard you use (UnleashX, XBMC, XBMC4Gamers) | The app's entry shows the green logo. If it still shows the old picture or none, reset the dashboard's thumbnail cache (or rename the folder) and look again |
| 2 | Hardware | Launch the app | It starts as before |

## NAS over SMB

[ADR 0018](adr/0018-smb-sources.md): installs from a NAS's shared folder, the NAS scan and PLAY.

**On the PC** (done, 2026-10-30). The core parts are unit-tested and fuzzed (`test_smb`, `test_netbios`, `test_folder_image`, the SMB parts of `test_credentials`, `test_map`, `test_map_edit`; `fuzz_smb`, `fuzz_netbios`, `fuzz_folder_image`, `fuzz_credentials`, `fuzz_map`). The client was run with `xcat-cli` against two servers on loopback: Impacket's `SimpleSMBServer` (SMB2, a user with a password) and a Go server (SMB2/3, guest), serving synthetic fixtures only:

| Check | Result |
|---|---|
| `smb-shares`, `smb-ls`, `smb-ls --tree` | Disk shares only; images and game folders found, two levels down; recycle bins and `@` folders left out |
| NT-hash login (Impacket), guest (Go), wrong password | Logged in; logged in; `XC_ERR_SMB_LOGIN` |
| `map` with `@smb` lines on both servers | Entries named as smb.md §3 says, game folders with a `/` |
| `install` of an xiso, a zip (Deflate) and a full-disc image | Byte for byte as the fixtures' manifests |
| `install` of game folders: 9 files, and 402 files in nested folders (15 MiB, empty files and folders, files of 2047–2049 bytes) | Identical to the folder (`diff -r`), but for the names smb.md §3.2 leaves out; 25 MB/s from the Go server, 0.9 MB/s from Impacket (its Python is the limit) |
| The server stopped mid-install and back within the reconnect | 1 reconnection, same bytes, install finishes |
| The server stopped mid-install for longer | Fails with `XC_ERR_NET_CONNECT`; running it again resumes (169 files kept, 233 written), identical result |
| The folder changed between the two attempts | Not continued into: installed to "Title (2)" (ADR 0008), the old folder left for the user |
| `smb-scan` of a /24 on loopback | Both servers found (127.0.0.1, 127.0.0.2) |
| A NAS that's off | Fails in 3 s (the probe), not 20 |

**On the console** (to do: lwIP and the scan can't be tested on the PC):

| Step | How | Pass |
|---|---|---|
| 1 | SETTINGS > NAS | `This Xbox: <its address>` at the bottom of the panel |
| 2 | Find a NAS on the network | The NAS is listed at the top by name and address within ~15 s |
| 3 | Type a NAS address: the NAS's IP | Asks for the user name |
| 4 | Log in with the console's NAS user; then, with X, a wrong password | Shares listed, "Login saved."; `E:\Apps\xcat\credentials` has the section (no password); "The NAS refused the user name or password" |
| 5 | B to the NAS page, then A on the NAS | Its shares, with no keyboard |
| 6 | Open shares and folders, check each opens the one highlighted; X on one, ADD | The question names that folder; `E:\xcat\map.txt` has its `@smb` line; GAMES lists its images and game folders, and their details name the folder they're in |
| 7 | Install an `.iso`, a `.zip` and a game folder | Each verified; `install.log` shows MB/s and 0 reconnections |
| 8 | Pull the network cable mid-install for 10 s, put it back, RETRY | Resumes; the result is verified |
| 9 | INSTALLED > A > PLAY | The game starts |
| 10 | Switch the NAS off, start the app | The list loads (3 s per `@smb` folder), installing from it says the NAS doesn't answer |

First run on the owner's console (0.10.0, 2026-11-01): scan, login, browse and install worked; the login wasn't kept unless a folder was added, a folder picked in the list could open another (the list was sorted for display, the names it opened weren't), and the menus didn't say which folder was meant. Fixed in 0.10.1; steps 1, 4–6 check it.

## Archives, apps and deleting (0.11.0)

[ADR 0018](adr/0018-smb-sources.md) decisions 9-11.

**On the PC** (done, 2026-11-01): `test_sevenz` (every method 7-Zip writes that's supported, read in pieces across buffer boundaries and backwards; an image streamed from 0 and from the middle; PPMd and passwords refused by name; the dictionary bound; damaged and cut archives; a folder through the image), `test_arc_folder` (root choice, clutter, made-up folders, refusals; a Python-made zip with Deflate and stored members, read back to front), the UI's delete and app questions (`test_ui_library`), and 20,000 mutated archives through `fuzz_sevenz`. `xcat-cli install`, from files and over SMB (Go server), of archives 7-Zip 25.01 made from synthetic folders:

| Archive | Result |
|---|---|
| `.7z` with one image: LZMA2, LZMA, stored; one with late files (`late.iso`) | Identical to the image's own install; stored read in place, the others streamed; SMB read exactly the archive's size |
| `.7z` with a 402-file game folder: solid (7-Zip's order, by extension), one block per file, BCJ on everything | Identical to the folder (`diff -r`), 15-16 MB/s; the solid one read its 15 MB once |
| `.7z` with PPMd, BCJ2, a password on the files, a password on the names | Refused before anything is written, naming the method or the password |
| `.7z` needing 15 MiB with `--max-dict 4` | Refused: "needs a 15 MiB dictionary; this console has room for 4 MiB" |
| `.7z` with a flipped byte, cut short, or random bytes | `XC_ERR_CORRUPT`, `XC_ERR_NOT_7Z` |
| `.zip` with a game folder (Deflate, in a top folder, with `__MACOSX` and a readme beside it); one with an app at its top (stored); two games; LZMA members | Identical; identical; "several games"; refused, naming the method |

**On the console** (to do):

| Step | How | Pass |
|---|---|---|
| 1 | Install a `.7z` image made at 7-Zip's Normal level from the NAS | Verified; `install.log` says "7z: room for a N MiB dictionary" and the MB/s |
| 2 | The same at Ultra level | Refused with both dictionary sizes |
| 3 | Install a zipped app from a NAS folder named `Apps` | The question says it looks like an app; it lands in `E:\Apps\<name>`; INSTALLED lists it; PLAY starts it |
| 4 | Stop an install with B; GAMES > A on it > DELETE > DELETE | The folder is gone; the game says nothing is installed |
| 5 | INSTALLED > A on an installed game > DELETE > KEEP, then DELETE > DELETE | Kept, then deleted; its saves in `E:\UDATA` are still there |

## FATX probe (M2)

No authoritative list of FATX naming rules exists, and a game's files can't be renamed on install, because the game opens them by name. So the rules in `core/src/fatx.c` ([fatx-names.md](formats/fatx-names.md)) come from the console's own kernel, measured by `app/fatx-probe`.

**What it tests**, under `E:\xcat-fatx-probe` (deleted before and after the run):

| Test | What |
|---|---|
| Name length | Files and directories with 40–43-character names |
| Characters | Every byte 0x01–0xFF (except `\`) inside a file name |
| Edge cases | Trailing dot, trailing space, leading space, `...`, `CON`, … |
| Case | Is lookup case-insensitive; can `CASEPROBE` be created next to `CaseProbe`? |
| Path length | Longest directory path and file path, excluding the drive |
| Entries | Files in one directory, up to 5,000 |
| File size (v4) | The largest file FATX stores: 4 GiB minus 2 clusters, minus 1 cluster, minus 1 cluster + 1 byte, minus 2 KiB, minus 1 byte. Each is set directly and also grown across the end by writing its last 1 MiB. Needs about 4.1 GB free on E: (skipped otherwise). Added after a 4,294,965,248-byte file failed at its end in the M4 test. |

**How to run it:**

1. `make docker-xbe` (or download the CI artifact) builds `app/fatx-probe/xcat-fatx-probe.iso` and `app/fatx-probe/bin/default.xbe`.
2. **xemu:** Machine → Load Disc → the ISO. It needs an HDD image with a formatted E: partition. **Hardware:** FTP `default.xbe` to e.g. `E:\Apps\xcat-fatx-probe\` and launch it.
3. Wait for "Done" (the entries and file-size tests take the longest), and note or photograph the summary.
4. Fetch `E:\xcat-fatx-probe.txt` over FTP (hardware). In xemu, the on-screen summary is enough unless something looks unexpected.

### Probe results

| Date | Where | Kernel | Name length (file/dir) | Rejected bytes | Case | Longest path (dir/file) | Entries | Report |
|---|---|---|---|---|---|---|---|---|
| 2026-10-13 | xemu v0.8.136 | 1.0.4627.1 | ≤ 42 / ≤ 42 (43 rejected) | 83 of 254: see below | lookup insensitive; other-case duplicate rejected | dir 250; file: see note | 4,096 (then rejected) | on-screen summary (probe v2, `ddc3b53`) |
| 2026-10-13 | Hardware (main console) | not recorded | same as xemu | same as xemu | same as xemu | same as xemu | same as xemu | reported by the owner as identical to the xemu run; report file not captured |
| 2026-10-15 | Hardware (main console) | 1.0.5838.1 | ≤ 42 / ≤ 42 (43 rejected) | 43 of 254: the control bytes and the 12 ASCII bytes below; **every byte ≥ 0x80 accepted** | same as xemu | dir 250; **file 250 ok, 251 rejected** | 4,096 (then `0xC00002EA`) | probe v4 report file. **File size:** grows to 4 GiB − 16 KiB, fails one byte beyond with `0xC0000102` ([fatx-names.md §4b](formats/fatx-names.md)) |

**Rejected bytes (xemu run):**

- `0x01`–`0x1F` (all control characters). `0x00` can't occur and `0x5C` (`\`) is the separator, so neither was tested.
- ASCII: `0x22 "`, `0x2A *`, `0x2B +`, `0x2C ,`, `0x2F /`, `0x3A :`, `0x3B ;`, `0x3C <`, `0x3D =`, `0x3E >`, `0x3F ?`, `0x7C |`. All other printable ASCII is accepted, including space, `[` `]` and `0x7F`.
- Bytes ≥ `0x80` (40, no obvious pattern): `84 86 87 89 8B 8F 91 9F A0 A1 A2 A5 A6 A9 AA AD AE B0 BF C0 C1 C2 C5 C6 C9 CA CD CE D0 DF E0 E1 E4 E5 E6 E7 E9 EF F1 FF`.

**Edge names:** trailing dot, trailing space, leading space, `a..b`, `...`, `CON` and `nul.txt` are all accepted.

**Path note:** paths are counted from the first `\` after `E:`. Directories nested to exactly 250 characters were created, and 251 was rejected. Every file the probe tried went inside the 250-character directory, so it was over the limit. A file path of exactly 250 wasn't tried directly. The limit is evidently on the whole path, and the hardware run should confirm a 250-character file path.

**Consequences for M2:** FATX's 250-character path limit is stricter than nxdk's winapi limit (259 including `E:`), so the app can use nxdk's file calls if it enforces 250 itself. A game is installable only if every path under `\Games\<title>\` fits in 250 characters, every name is valid, and no directory has more than 4,096 entries.

## Benchmarks

| Date | Measurement | Setup | Result |
|---|---|---|---|
| _pending_ | PC → archive.org download speed (HANDOFF §8.1) | `curl -r 0-104857599` against a storage server | |
