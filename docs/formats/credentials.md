# Credentials file format (`xcat-credentials`), version 1

**Status:** Normative. §3–§7 are implemented by `xcat/credentials.h` (M3) and used by `xcat-cli get --credentials`; the console loads the file with the install job (M4), and `tools/xcat-login` comes with the archive.org source (M6). The reasons behind it are in [ADR 0007](../adr/0007-scoped-user-credentials.md).

The credentials file holds login tokens for sources that need them, and says exactly which hosts may receive each one. It is separate from the [map](map.md) on purpose: maps are shared and included, and credentials never are.

The words **must**, **must not**, **should** and **may** are used as in RFC 2119.

## 1. Example

```
# xcat-credentials v1

# Session for my own archive.org account (written by the on-box login, tools/xcat-login, or by hand)
[archive]
hosts  = archive.org, *.archive.org
cookie = logged-in-user=<user>; logged-in-sig=<token>

# NAS on the LAN that uses Basic auth over plain HTTP
[nas]
hosts      = 192.168.1.20:8080
allow_http = yes
header     = Authorization: Basic <base64 of user:password for the NAS>

# The NAS's shared folders (SMB, smb.md): written by SETTINGS > NAS, or by xcat-cli smb-hash
[nas-smb]
hosts      = 192.168.1.50
smb_user   = xbox
smb_nthash = <32 hex digits: the NT hash of the user's password>
```

Placeholders in `<…>` stand for real values. Never commit or share a real credentials file.

## 2. Location and loading

- On the console: `E:\Apps\xcat\credentials`, with no extension (not `credentials.txt`; the app says so when it finds only that). On a PC, `xcat-cli` takes the path as an option.
- The file is **only** read from local storage. It **must not** be loaded from a URL, through a map's `@include`, or from any path a map names.
- The file is read once, when an install job starts. Edits apply to the next job.
- If the file is missing, no credentials are used. That isn't an error.

## 3. Syntax

1. UTF-8 text, optional leading BOM ignored, LF or CRLF line endings, at most **64 KiB** in total and **4096 bytes** per line.
2. Leading and trailing whitespace is ignored on every line. Blank lines, and lines starting with `#`, are skipped. There are no inline comments, because values may contain `#`.
3. The first non-blank line **must** be the header `# xcat-credentials v<MAJOR>[.<MINOR>]`. Versioning follows [map.md §3](map.md#3-header-and-versioning): a newer major version is rejected, and unknown keys are rejected even within the same major version (§7).
4. `[name]` starts a section. `name` is 1–32 characters from `A–Z a–z 0–9 _ -`, unique (case-insensitively), and is how the section is referred to in messages and logs.
5. Inside a section, every other line is `key = value`, split at the first `=`, with whitespace around both sides trimmed. At most **32** sections.

## 4. Keys

| Key | Required | Repeatable | Meaning |
|---|---|---|---|
| `hosts` | yes | no | Comma-separated host patterns, at most **16**, that may receive this section's credentials (§5) |
| `cookie` | one of `cookie`/`header`/`smb_user` | no | Value sent in the `Cookie` request header, e.g. `a=1; b=2` |
| `header` | one of `cookie`/`header`/`smb_user` | yes, up to 16 | `Name: value`, sent as a request header |
| `allow_http` | no | no | `yes` or `no` (default `no`). See §6 |
| `smb_user` | one of `cookie`/`header`/`smb_user` | no | The user name an SMB login uses (§4.1) |
| `smb_domain` | no | no | Its domain or workgroup, if the NAS needs one (§4.1) |
| `smb_nthash` | with `smb_user` | no | The NT hash of its password (§4.1) |

Value rules:

- Values **must not** contain control characters (bytes below `0x20`, or `0x7F`).
- `header` names are HTTP tokens (`A–Z a–z 0–9` and ``!#$%&'*+-.^_`|~``). These names are **forbidden** (case-insensitive), because setting them would break or smuggle requests: `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Range`, `Cookie` (use `cookie`). The same name may not be given twice in a section.
- Query-string tokens are not supported ([ADR 0007](../adr/0007-scoped-user-credentials.md) decision 4).

### 4.1 SMB logins

A section with `smb_user` is a login for the NAS shared folders (`smb://`, [smb.md §4](smb.md#4-logging-in), [ADR 0018](../adr/0018-smb-sources.md)) on the hosts it names:

- `smb_user` is 1–128 bytes and `smb_domain` 1–64, neither with `\ / : @ "`. `smb_domain` needs `smb_user`.
- `smb_nthash` is exactly 32 hex digits, either case: MD4 of the password in UTF-16LE. It's required with `smb_user`, and is the only form a password is kept in. It still logs in to SMB servers as that user (§8).
- A section may hold both HTTP credentials (`cookie`, `header`) and an SMB login: one machine can serve both. HTTP requests get only the former, SMB connections only the latter.
- An `smb://` address's host and port are matched against `hosts` as in §5; `*.` patterns match too. The port of a pattern without one is any port. No section matches: a guest login.

## 5. Host patterns

```
pattern  = host [ ":" port ]
host     = hostname / "*." hostname / ipv4
```

- `archive.org` matches exactly that host.
- `*.archive.org` matches any subdomain at any depth (`dn720004.ca.archive.org`), but **not** `archive.org` itself. List both when both are wanted.
- An IPv4 literal matches only that address. IPv6 literals are not supported in v1.
- No other wildcards. A pattern of `*` alone, or one with `*` anywhere but the leading `*.`, is invalid.
- Matching is case-insensitive and compares the host from the request URL, after redirects. Without `:port`, any port matches. With it, only that port.
- **No overlap.** Two sections whose patterns could both match one host (e.g. `*.archive.org` and `ia800.archive.org`) make the file invalid, when both have HTTP credentials or both have an SMB login. There's never a question of which credential is sent. A section with only HTTP credentials and one with only an SMB login may name the same host.

## 6. When credentials are sent

This section is about HTTP: a section's `cookie` and `header`. Its SMB login is used only to connect to an `smb://` address whose host and port match (§4.1, [smb.md §4](smb.md#4-logging-in)), and never sent in an HTTP request.

For **every request hop**, including each redirect target, the HTTP client attaches a section's `cookie` and `header` values only if all of these hold:

1. the URL's host and port match one of the section's `hosts` patterns; and
2. the scheme is `https`. Plain `http` is allowed only if the section has `allow_http = yes` **and** the host is an IPv4 literal in a private range (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16).

Otherwise the request goes out without credentials. In particular:

- a redirect to a host outside the scope is followed **without** credentials;
- a redirect from `https` to `http` is followed without credentials, unless rule 2 allows it for that host.

Cookies the server sets (`Set-Cookie`) are ignored and never stored. The only cookies sent are the ones in this file.

## 7. Errors

Because a mistake here could send a secret to the wrong host, the file is **all-or-nothing**. Any problem rejects the **whole file**. The problem is reported with its line number, and installs proceed without any credentials. Problems include:

- unreadable or oversized file, a missing or malformed header, an unsupported major version;
- a malformed line, an unknown key, a duplicate key (other than `header`), a missing `hosts`, or a section with none of `cookie`, `header` and `smb_user`;
- `smb_user` without `smb_nthash` or the other way round, `smb_domain` without `smb_user`, an invalid user or domain name, an `smb_nthash` that isn't 32 hex digits;
- an invalid section name, host pattern, header name or value; a forbidden header name;
- overlapping host patterns across sections.

An HTTP `401` or `403` from a host that had credentials attached is reported as "login missing or expired for `[name]`". It is never retried with different credentials or workarounds ([ADR 0006](../adr/0006-no-links-no-bypass.md)).

## 8. Handling secrets

- Logs and on-screen messages may name a section (`[archive]`), never a value.
- The app writes this file only when the user logs in or out on the console, or removes a login on SETTINGS' Authentication page ([ADR 0010](../adr/0010-on-box-login-required-for-beta.md), M8): it writes, replaces or removes that service's one section, atomically (temporary file, then rename), and keeps every other section byte for byte. How is in [login.md §5](login.md#5-writing-the-credentials-file); a file that doesn't parse is left alone. The optional PC helper `tools/xcat-login` (M6) does the same. Neither ever stores a password: it's used once for the login request over HTTPS and wiped from memory. SETTINGS > NAS writes or replaces an SMB login's section the same way when a NAS folder is chosen (or removes it when the folder is used as a guest); the password typed there is used once to compute `smb_nthash` and wiped.
- Anyone who can read the console's drive can read this file. Change the dashboard's default FTP password, and revoke a session (log out everywhere, or change the account password) if the console is lost or shared.

## 9. Changes

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-13 | First version (design). |
| 1.0 | 2026-10-15 | Implemented. The limit of 16 host patterns per section is now stated (§4); nothing else changed. |
| 1.0 | 2026-10-15 | §8: the app may write a login's section (on-box login, ADR 0010). The file format is unchanged. |
| 1.0 | 2026-10-27 | §8: the on-box login is built; how it writes the file is in login.md §5. The file format is unchanged. |
| 1.0 | 2026-10-27 | §8: SETTINGS' Authentication page can remove any one section the user picks, the same way (every other byte kept). The file format is unchanged. |
| 1.0 | 2026-10-29 | §4, §4.1: SMB logins (`smb_user`, `smb_domain`, `smb_nthash`, [ADR 0018](../adr/0018-smb-sources.md)); §5: overlap is checked per kind; §6, §7, §8 updated. Older readers reject a file with these keys (unknown keys, §7), so a console on an older version uses no credentials until it's updated. |
