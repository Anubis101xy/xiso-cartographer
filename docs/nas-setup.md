# Games from a NAS: setting it up

How to install games on the console from a NAS's shared folder, with a TerraMaster F4 as the example (TOS 5, 6 and 7 name things much the same). Any NAS, or a Windows or Linux PC sharing a folder, works the same way: the console speaks SMB2/3, the protocol Windows file sharing uses. The details are in [formats/smb.md](formats/smb.md).

The console copies the game to its own drive (E:\Games by default), then starts it from there. Games don't run from the NAS itself: no app can do that on the original Xbox.

## 1. On the NAS

1. **SMB on.** TOS: Control Panel > File Services > SMB: enable the SMB service. Under Advanced, the minimum SMB version must be SMB2 or older and the maximum SMB2 or newer (the defaults are fine). The console doesn't speak SMB1.
2. **A user for the console.** Control Panel > User: add a user, say `xbox`, with a password. Then Control Panel > Shared Folder > the share your games are in > Edit > Permissions: give `xbox` **Read only** there and nothing elsewhere.
   - Why its own user: the console keeps a hash of the password (not the password itself) in `E:\Apps\xcat\credentials`, and that hash is enough to log in to the NAS as that user. Anyone with FTP access to the console could read it. A read-only user that sees only the games share limits what that's worth.
   - A guest login works too, if the NAS allows guest access to that share (leave the user name empty on the console).
3. **A fixed address.** In your router, reserve the NAS's current IP address for it (DHCP reservation), or give it a static one in TOS (Control Panel > Network). The map stores the address the console found, so it must not change.

### What the console installs

- Disc images: `.iso`, `.xiso`, `.xiso.iso` (Redump full-disc images and trimmed xisos both work).
- Zips holding one image: `.zip` (stored or Deflate).
- 7z archives holding one image: `.7z`, unpacked as it installs, with nothing written to a temporary file. LZMA and LZMA2 (7-Zip's usual methods) work. The console has to hold the archive's dictionary in memory: 7-Zip's **Normal** level (16 MB) always fits, **Maximum** (32 MB) usually does, **Ultra** (64 MB) doesn't. If one doesn't fit, the message says how much it needs and how much the Xbox has; repack it at Normal level, or extract it on the NAS.
- Zips and 7z archives holding a game or app folder (a `default.xbe` at their top, or in a folder inside, like `My App/default.xbe`): that folder is installed. Whatever sits beside it (a readme) and macOS clutter are left out.
- Extracted game folders: any folder with a `default.xbe` in it, copied file by file with the same resume and checks. NAS clutter (`.DS_Store`, `._` files, `@eaDir`, `#recycle`) is skipped.
- Images and game folders can sit in the folder you pick, or one folder down (`Xbox\Halo (USA)\Halo (USA).iso`, `Xbox\Halo (USA)\default.xbe`). If the folder you pick is itself a game folder, it's listed as that one game.
- Not supported: `.rar` archives, compressed `.cci` / `.cso` images, and 7z archives with a password or made with BCJ2 or PPMd (7-Zip's Ultra level for programs, or a PPMd choice). RAR, CCI and CSO files don't show up; the others say why when you install them.
- Names longer than FATX allows (42 characters per name) are refused with the list of problems: rename it on the NAS. In a game folder, two names that differ only in upper and lower case (`Data` and `data`) can't both go on the Xbox's drive either; the log names them.

## 2. On the console

1. Copy the `xiso-cartographer` folder from the zip to the console, for example to `E:\Apps\xcat\`, and keep `fonts\` and `ui\` next to `default.xbe`. Start it from your dashboard.
2. **SETTINGS > NAS.** The network starts, and the bottom of the right-hand panel shows the console's own address (`This Xbox: 192.168.1.23`), so you can see it's on the same network as the NAS. NASes you've logged in to before are listed first.
3. **Find a NAS on the network** asks every address of the console's network for file sharing, about ten seconds on a home network, and lists what it found at the top, by name (`TNAS-...`) and address. If your NAS isn't listed, use **Type a NAS address** with its IP address.
4. Pick the NAS and press A. The first time, enter the user name (`xbox`), then its password, on the on-screen keyboard; an empty user name logs in as a guest. As soon as the NAS accepts it, the login is saved (only the password's hash, in `E:\Apps\xcat\credentials`): next time, A on the NAS goes straight to its shared folders. **X** on a NAS logs in as someone else, or again after a password change.
5. **Browse:** A opens a shared folder or a folder, B goes up. The right-hand panel always describes the highlighted line. Two ways to add a folder:
   - open it, then pick **Add this folder to GAMES** at the top (it says **Add this game to GAMES** if the folder is itself a game, with a `default.xbe`); or
   - highlight it and press **X**, without opening it (on the first level, X adds a whole shared folder).

   The question that follows names exactly the folder that will be added; ADD confirms.
6. The console writes an `@smb` line in your map (`E:\xcat\map.txt`), lists the games and takes you to **GAMES**. Each game's details say where on the NAS it is.
7. Pick a game and install it. A progress screen shows the copy, then it reads every file back to check it. If the network drops, it resumes where it stopped next time you install the same game.
8. Installed games: **A > PLAY** starts the game. In GAMES an installed game also offers INSTALL AGAIN; in INSTALLED, DELETE. INSTALLED's left-hand panel shows how much is free on the drive the games go to (`E: 412 GB free`, a bar of how full it is, `of 931 GB`), and on E: too if they go to F: or G:.

### The library screen

- **The top panel** shows the console's address (`XBOX 192.168.1.23`), the NAS of the highlighted game (`NAS 192.168.1.50`, or `off` in orange if its folders didn't list when the app started), and the free space on the drive games go to with a bar of how full it is.
- **GAMES is in alphabetical order** (numbers by value: Halo 2 before Halo 10). A name too long for its line slides along after a moment so you can read the rest.
- **Pictures and details.** Rest on a game for a moment and its details show a picture and what the game's own files say; the footer says it's reading while it asks the NAS. What's looked for, beside the game on the NAS:

  | Where the game is | Picture | Details |
  |---|---|---|
  | An image or archive among others in the folder you added | `Halo (USA).jpg`, `.png` or `.tbn` next to `Halo (USA).iso` | The disc image's `default.xbe` (not an archive's) |
  | An image or archive in a folder of its own (`Halo (USA)\Halo (USA).iso`) | The same, then that folder's `_resources\artwork\poster.jpg`, `default.tbn`, `cover.jpg`/`.png`, `folder.jpg`/`.png` | The same, and `_resources\default.xml` |
  | An extracted game folder | Its `_resources\artwork\poster.jpg`, `default.tbn`, `cover.jpg`/`.png`, `folder.jpg`/`.png` | Its `default.xbe` and `_resources\default.xml` |

  With no picture beside it, the title image inside the game's `default.xbe` is shown (the one dashboards show). `default.xbe` gives the real title, title ID, region (NTSC-U, NTSC-J, PAL) and disc number. `_resources\default.xml` is XBMC4Gamers' synopsis: the title, developer, publisher, genre, year, players, ESRB, rating and an overview. If you already use XBMC4Gamers artwork packs, copy each game's `_resources` folder next to its `default.xbe` (or next to its image, in the image's own folder) and they show up here. Pictures can be JPEG (including progressive) or PNG, up to 4 MB and 2048 pixels a side. **White** and **Black** scroll details too long for the panel.
- INSTALLED shows the same for each install, from its folder on the console: its `default.xbe`, and `_resources` if it has one.

### Apps

The install question has a second button: **AS AN APP** puts it in `E:\Apps` instead of the games folder. Anything in a NAS folder called `Apps`, `Applications`, `Homebrew`, `Emulators` or `Dashboards` goes to `E:\Apps` by default (its second button is **AS A GAME**), so keeping your apps in a folder with one of those names saves a press. Installed apps are listed in INSTALLED with the games, and PLAY starts them.

### When an install fails

An install that stopped or failed keeps what it wrote, so installing it again carries on. If you'd rather have the space back:

- in **GAMES**, A on it asks **CARRY ON / DELETE / CANCEL**;
- in **INSTALLED**, A on an unfinished one asks to delete it, and on an installed one PLAY's middle button is **DELETE**.

Either way it asks once more, with **KEEP** highlighted. Only folders this app installed can be deleted this way, and saved games stay (they're in `E:\UDATA`, not in the game's folder).

You can add more than one folder (or more than one NAS): repeat 2–5.

## 3. When it doesn't work

| What you see | What to check |
|---|---|
| The scan finds nothing | `This Xbox:` at the bottom of the NAS page shows an address on the same network as the NAS (the same first three numbers on a home network). If it says the Xbox isn't on the network, check the cable and the dashboard's network settings. SMB is on. Type the address instead. |
| "The NAS refused the user name or password", or "refused the saved login" | The user name and password; whether the account is enabled. For a domain user, type `DOMAIN\user`. X on the NAS types them again. |
| "The NAS has no shared folder of that name" | The share was renamed or removed: add the folder again. |
| "This login may not open that folder or file" | The console's user can't read that share or folder: give it Read only in the share's permissions. |
| "The NAS (SMB) refused or failed the request" | SMB2 or SMB3 is allowed (File Services > SMB > Advanced). The log has the NAS's own words. |
| "The server stopped responding" | The NAS's disks were asleep (it waits 20 seconds for them) or the network dropped. Try again: the install resumes. |
| The NAS is off | Its folder costs 3 seconds when the list loads, and its games can't be installed until it's back. The top panel says `off`. |
| No picture for a game | The picture's name: `<image name>.jpg` beside the image (same name, without `.iso`), or in the game's own folder. It must be a JPEG or PNG (the file's content counts, not its name) of at most 4 MB. `E:\xcat\install.log` has a line for each game read, saying what was found. Archives' own `default.xbe` isn't read: put a picture beside them. |

The console writes what happened to `E:\xcat\install.log`; FTP it off and send it with any report. It never contains the password or the hash.

## 4. What's stored where

| File | What |
|---|---|
| `E:\xcat\map.txt` | `@smb smb://192.168.1.50/Games/Xbox`: the folders the games are listed from. Edit or remove the line in SETTINGS > Map sources. |
| `E:\Apps\xcat\credentials` | A section for the NAS's address with `smb_user` and `smb_nthash`. Remove it in SETTINGS > Authentication. |
| `E:\Games\<name>\` | The installed games, with `.xcat-installed` (where it came from, its title ID). |
