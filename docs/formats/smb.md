# NAS shared folders over SMB (`smb://`)

**Status:** Normative. Implemented by `xcat/smb.h` (`core/src/smb.c`: addresses, image names, NT hashes, listings), `xcat/folder_image.h` (game folders as images), `xcat/netbios.h` (`core/src/netbios.c`: node status), the map's `@smb` directive ([map.md §5.4](map.md#54-smb-folder)), the credentials file's SMB keys ([credentials.md §4.1](credentials.md#41-smb-logins)), and the SMB client and NAS scan in `platform/smb` (`xcat/smb_client.h`, `xcat/smb_scan.h`) over libsmb2 ([ADR 0018](../adr/0018-smb-sources.md)). `xcat-cli smb-scan`, `smb-shares`, `smb-ls` and `smb-hash` run them on a PC.

A NAS (or any PC) that shares a folder over SMB, the protocol Windows file sharing uses, can hold the user's images. The console lists the images in a shared folder, logs in with the user's own account or as a guest, and installs from there like from any other source: stream-extracted, resumable and verified ([ADR 0004](../adr/0004-streaming-install.md)).

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Addresses

```
smb-url = "smb://" host [ ":" port ] "/" share *( "/" name ) [ "/" ]
```

Examples: `smb://192.168.1.50/Games/Xbox/Halo (USA).iso`, `smb://tnas.lan/Games/Xbox` (a folder, for `@smb`), `smb://tnas.lan/Games/Xbox/Fable (USA)/` (an extracted game folder, §3.1).

1. The scheme is `smb`, in any case. At most **1024 bytes** in all. No NUL.
2. There's no user name or password in the address (`smb://user@nas/...` is invalid): logins live in the credentials file (§4).
3. There's no query or fragment: a literal `?` or `#` makes the address invalid. A `#` in a name is written `%23`.
4. `%XX` escapes are decoded in the share and in each name. An escaped `/` (`%2F`) or anything §1.2–§1.3 forbids is invalid once decoded.

### 1.1 Host and port

- A host name of letters, digits, `-` and `_` in dot-separated labels of 1–63 characters, at most 253 bytes, compared lowercased; or an IPv4 address of four decimal numbers 0–255 without leading zeros. A host of only digits and dots must be such an address. IPv6 isn't supported.
- The port is 1–65535, without leading zeros. It defaults to **445**.

### 1.2 Share

The first path segment: 1–80 bytes once decoded, valid UTF-8, no control character and none of `\ / : * ? " < > |`.

### 1.3 Folders and file

Each further segment is a folder or file name: 1–255 bytes once decoded, valid UTF-8, no control character, none of `\ / : * ? " < > |`, and not `.` or `..`. No empty segments (`//`), but one trailing `/` is allowed. The decoded path inside the share, its names joined with `/`, is at most **768 bytes**.

A map entry's address that ends in `/` names an extracted game folder (§3.1), and may then be a whole share (`smb://nas/Halo/`); any other entry names an image file. An `@smb` folder's trailing `/` means nothing.

### 1.4 Canonical form

`smb://` + the host lowercased + `:port` if it isn't 445 + `/` + the share + `/` + the path, with `%` written `%25` and `#` written `%23`; every other byte as itself (spaces and UTF-8 included). The canonical form parses back to the same address. The map writes generated entries and `@smb` lines in it.

## 2. Shares

Listing a NAS's shares (SETTINGS > NAS, `xcat-cli smb-shares`) asks its `IPC$` share for them (`NetShareEnum`, level 1). Only disk shares are listed: not printers, devices, IPC, or hidden shares (the hidden flag, or a name ending in `$`). At most 64.

## 3. Listing a folder for `@smb`

The games of a folder are its images and game folders, and those of the folders in it, **2 levels**: the folder itself and the folders directly inside it. That covers a folder of images, a folder of one folder per image, and a folder of extracted games.

- A file is an image if its name ends in `.xiso.iso`, `.iso`, `.xiso` or `.zip`, in any case. Its entry name is the file name without that ending, the longest that matches (`Halo (USA).xiso.iso` → `Halo (USA)`), as for archive.org ([archiveorg.md §4](archiveorg.md#4-the-entry)). A name that leaves nothing (`.iso`) isn't an image. Other files are left out without a word.
- Folders whose names start with `.` or `@`, and `#recycle`, `$RECYCLE.BIN`, `#snapshot` and `lost+found` (any case), aren't gone into: NAS systems keep their own data there.
- A folder the login may not open, or that disappears while it's listed, is skipped.
- Names an address can't hold (§1.3) are left out.
- At most **10,000** images (the map's entry limit, [map.md §6](map.md#6-limits)); past that the listing stops and the map warns.

Each image becomes a map entry keyed by its name, with the size the folder listed and the canonical address of the file (§1.4). The size is checked against the file's when it's installed (`XC_ERR_SOURCE_SIZE` if they differ).

### 3.1 Game folders

A folder that has a file named `default.xbe` (any case) directly in it is an **extracted game folder**: a game copied out of its image, as dashboards and FTP leave them.

- It becomes one map entry, named after the folder (the `@smb` folder itself, if that's the one with the `default.xbe`: its last name, or the share's), with no size, and the folder's canonical address with a `/` at the end.
- Nothing inside it is listed: not its images, not its folders.
- A folder at the second level is a game folder by the same test, so a folder of game folders lists each of them.

### 3.2 Installing a game folder

Installing a game folder copies its files as if they were an image: the folder is listed, every level of it, and the listing becomes the directory tables of an image that exists only in memory ([folder-image.md](folder-image.md)), which installs like any other: the same FATX checks and refusals (ADR 0008), the same journal and resume, and every file read back and checked.

- Left out, as NAS systems and other computers leave them in folders: names starting with `.` (`.DS_Store`, `._*`, `.xcat-installed`), and folders the listing leaves out (§3: names starting with `@`, the recycle bins and snapshots).
- Anything else that the image can't hold fails the install with `XC_ERR_GAME_FOLDER`, and the log names it: a name SMB addresses can't hold (§1.3), two names in one folder that differ only in case, a file of 4 GiB or more, a path over 768 bytes on the share, more than 65,536 files and folders ([folder-image.md §2](folder-image.md#2-the-listing)).
- A folder that has no `default.xbe` once listed isn't installed (`XC_ERR_GAME_FOLDER`).
- Each file is opened when the install gets to it and closed after, so one file is open at a time; a file whose size changed since the listing fails with `XC_ERR_SOURCE_SIZE`. A dropped connection reconnects as for an image (§4).
- The install's ETag is the listing's validator ([folder-image.md §5](folder-image.md#5-validator)): an unfinished install of a folder that has changed since (a file added, removed, resized or rewritten) isn't continued; the game goes into a new folder ("Title (2)", ADR 0008).
- No image hashes (journal.md §7): there's no image to hash. Every file is still checked against its CRC-32 after it's written.

## 4. Logging in

- A NAS with a credentials section that has `smb_user` (credentials.md §4.1) is logged in to with that user, its `smb_domain` if there is one, and its `smb_nthash`: NTLMv2 inside SPNEGO, the way Windows logs in. Without one, the console logs in as the user `guest` with no password, which a NAS allows only if guest access is on.
- **The password is never stored.** What is kept is the NT hash: MD4 of the password in UTF-16LE (at most 256 bytes of UTF-8), as 32 hex digits. NTLM logins are made from it alone, so it's as good as the password for logging in to SMB servers, but the password itself (which may be used elsewhere) can't be read back from it. Anyone who can read the console's drive can read the hash: give the console its own NAS user with read-only access to the games share.
- SMB 2.02 to 3.1.1 are offered. Signing and encryption are used when the server asks for them. SMB1 isn't supported.
- Each request waits up to **20 seconds** for its answer (a NAS may need that long to spin up its disks). Before connecting to an IPv4 address, the console checks that something accepts a connection on the port, for up to **3 seconds**, so a NAS that's switched off fails fast.
- If a read fails because the connection dropped or stopped answering, the client connects, logs in and opens the file again, then retries the read, once per failure. A file that changed size meanwhile is refused (`XC_ERR_SOURCE_SIZE`).

What a failure means for the user (`xcat/error.h`):

| Server's answer | Error |
|---|---|
| `STATUS_LOGON_FAILURE`, `WRONG_PASSWORD`, `NO_SUCH_USER`, account disabled, locked out, expired or restricted, password expired or to be changed, outside logon hours | `XC_ERR_SMB_LOGIN` |
| `STATUS_BAD_NETWORK_NAME` | `XC_ERR_SMB_SHARE` |
| `STATUS_ACCESS_DENIED`, `NETWORK_ACCESS_DENIED` | `XC_ERR_SMB_DENIED` |
| `OBJECT_NAME_NOT_FOUND`, `OBJECT_PATH_NOT_FOUND`, `NO_SUCH_FILE`, `NOT_FOUND` | `XC_ERR_SMB_NOT_FOUND` |
| No answer to connect, refused | `XC_ERR_NET_CONNECT` |
| No answer in time | `XC_ERR_NET_TIMEOUT` |
| The connection broke | `XC_ERR_NET_IO` |
| Anything else | `XC_ERR_SMB` (the log has the server's words) |

## 5. Finding the NAS

SETTINGS > NAS > Scan the network (and `xcat-cli smb-scan`) finds SMB servers on the console's own network:

1. **Which addresses:** every host address of the console's network (its address and mask), but never more than its /24: a wider network scans the /24 around the console's address. Not the network's own address, its broadcast address, or the console's.
2. **TCP 445:** each address is asked for a connection to port 445, **16 at a time** on the console (lwIP's socket and ARP tables are sized for it: `platform/nxdk/lwip/lwipopts.h`), each batch waiting up to **600 ms**. An address that accepts is an SMB server; the connection is closed at once, before anything is sent.
3. **Names:** each server found is sent one NetBIOS node status request (RFC 1002 §4.2.17: the name `*` padded with NULs, type NBSTAT) on UDP port 137, all within **800 ms**. An answer to that request (same transaction ID, a response, no error) gives the server's name: its unique name with suffix `0x20` (the file server), else its unique `0x00` name; and its workgroup, its group `0x00` name. Names are cut at 15 bytes, trimmed of trailing spaces, and shown as printable ASCII (`?` for anything else). A server that doesn't answer is shown by its address.
4. At most 64 servers are kept, in address order.

Nothing else is sent: no broadcast, no multicast, nothing outside the network. On a /24 the scan takes about ten seconds.

## 6. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-29 | First version (ADR 0018). |
| 1.1 | 2026-10-30 | §3.1, §3.2: extracted game folders, listed and installed ([folder-image.md](folder-image.md)); §1.3: a trailing `/` on an entry names one. |
