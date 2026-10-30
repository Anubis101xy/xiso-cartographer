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
- Extracted game folders: any folder with a `default.xbe` in it, copied file by file with the same resume and checks. NAS clutter (`.DS_Store`, `._` files, `@eaDir`, `#recycle`) is skipped.
- Images and game folders can sit in the folder you pick, or one folder down (`Xbox\Halo (USA)\Halo (USA).iso`, `Xbox\Halo (USA)\default.xbe`). If the folder you pick is itself a game folder, it's listed as that one game.
- Not yet: compressed `.cci` / `.cso` images. They don't show up.
- Names longer than FATX allows (42 characters per name) are refused with the list of problems: rename it on the NAS. In a game folder, two names that differ only in upper and lower case (`Data` and `data`) can't both go on the Xbox's drive either; the log names them.

## 2. On the console

1. Copy the `xiso-cartographer` folder from the zip to the console, for example to `E:\Apps\xcat\`, and keep `fonts\` and `ui\` next to `default.xbe`. Start it from your dashboard.
2. **SETTINGS > NAS > Scan the network.** It asks every address of the console's network for a file sharing service, about ten seconds on a home network, and lists what it found by name (`TNAS-...`) and address. If your NAS isn't listed, use **Type an address** with its IP address.
3. Pick the NAS. Enter the user name (`xbox`), then its password, on the on-screen keyboard. An empty user name logs in as a guest. The password is only used to compute the hash and is wiped.
4. **Browse:** the NAS's shares are listed. A opens one, B goes up. Go to the folder with your games, then pick **Use this folder** at the top and confirm with ADD.
5. The console writes the login (`E:\Apps\xcat\credentials`) and an `@smb` line in your map (`E:\xcat\map.txt`), then lists the games: they're in **GAMES** now.
6. Pick a game and install it. A progress screen shows the copy, then it reads every file back to check it. If the network drops, it resumes where it stopped next time you install the same game.
7. Installed games: **A > PLAY** starts the game. In GAMES an installed game also offers INSTALL AGAIN.

You can add more than one folder (or more than one NAS): repeat 2–4.

## 3. When it doesn't work

| What you see | What to check |
|---|---|
| The scan finds nothing | The console has an IP address (the dashboard's network settings), the NAS is on the same network, SMB is on. Type the address instead. |
| "The NAS refused the user name or password" | The user name and password; whether the account is enabled. For a domain user, type `DOMAIN\user`. |
| "The NAS has no shared folder of that name" | The share was renamed or removed: add the folder again. |
| "This login may not open that folder or file" | The console's user can't read that share or folder: give it Read only in the share's permissions. |
| "The NAS (SMB) refused or failed the request" | SMB2 or SMB3 is allowed (File Services > SMB > Advanced). The log has the NAS's own words. |
| "The server stopped responding" | The NAS's disks were asleep (it waits 20 seconds for them) or the network dropped. Try again: the install resumes. |
| The NAS is off | Its folder costs 3 seconds when the list loads, and its games can't be installed until it's back. |

The console writes what happened to `E:\xcat\install.log`; FTP it off and send it with any report. It never contains the password or the hash.

## 4. What's stored where

| File | What |
|---|---|
| `E:\xcat\map.txt` | `@smb smb://192.168.1.50/Games/Xbox`: the folders the games are listed from. Edit or remove the line in SETTINGS > Map sources. |
| `E:\Apps\xcat\credentials` | A section for the NAS's address with `smb_user` and `smb_nthash`. Remove it in SETTINGS > Authentication. |
| `E:\Games\<name>\` | The installed games, with `.xcat-installed` (where it came from, its title ID). |
