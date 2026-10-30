# libsmb2 in xiso-cartographer

[libsmb2](https://github.com/sahlberg/libsmb2) is Ronnie Sahlberg's SMB2/3 client library, under the GNU Lesser General Public License 2.1 or later (`../COPYING`, `../LICENCE-LGPL-2.1.txt`). xiso-cartographer uses it for NAS shared folders ([ADR 0018](../../../docs/adr/0018-smb-sources.md), [smb.md](../../../docs/formats/smb.md)).

## What's here

- `../lib/`, `../include/`, `../cmake/ConfigureChecks.cmake`, `../cmake/config.h.cmake`, `../README`, the licences: upstream commit **`727feaaa6d0f4fed1bae57745fbc2c5db9a30e64`** (2026-10-25), with `lib/aes_apple.[ch]` and `lib/krb5-wrapper.c` left out (Apple CommonCrypto and Kerberos aren't used), and with the nxdk port below applied.
- `nxdk.patch`: the port, as a diff against that commit (`patch -p1` in a clean copy of the commit reproduces `../lib` and `../include` exactly).
- `nxdk/config.h`: libsmb2's `config.h` for nxdk, written by hand.

It's vendored rather than a submodule because of the port. To update it: copy the same files from a newer upstream commit, apply `nxdk.patch` (fix what no longer applies), regenerate the patch, and update the commit here and in `THIRD_PARTY.md`.

## The nxdk port (`nxdk.patch`)

nxdk's clang targets `i386-pc-win32`, so it defines `_WIN32` and `_MSC_VER`, but its sockets are lwIP's BSD API with `errno`, not Winsock. The port:

1. `lib/compat.h`, `lib/compat.c`: an `NXDK` platform like upstream's `PICO_PLATFORM` (lwIP's `netdb.h` and `sockets.h`, whose macros map `socket`, `poll`, `readv`, `writev`, `close`... to `lwip_*`), `EAI_AGAIN`, the `open()` flags libsmb2 maps to SMB create options, and `getlogin_r`, `gethostname`, `getpid`, `random`, `srandom`, which nxdk's C library lacks.
2. `lib/socket.c`: its `_WIN32` (Winsock: `WSAGetLastError`, `unsigned long` iovec lengths) code paths are not taken on nxdk.
3. `include/portable-endian.h`: little-endian x86 with clang's byte-swap builtins on nxdk, not the Windows path.
4. `lib/md5.h`, `lib/hmac-md5.h`: `<sys/types.h>` only where it exists.
5. `lib/init.c`: `smb2_random_bytes()` asks `SMB2_RANDOM_BYTES_HOOK` first when it's defined. The XBE defines it as `xc_nx_smb2_random` (`platform/nxdk/src/nx_smb.c`), the console's entropy pool (ADR 0012 §6), since nxdk has no random generator: the NTLMv2 client challenge and SMB3 nonces must not be predictable.

None of it changes libsmb2 on other platforms.

## Building

- XBE: `app/smb.mk` (the sources, `-DHAVE_CONFIG_H`, `nxdk/config.h`, the `NEED_*` flags for the helpers in 1., the random hook, `-O2 -w`).
- PC: `cmake/libsmb2.cmake` (Linux and macOS; a `config.h` from the checks libsmb2's own CMake makes). Windows hosts build without SMB.

## Licence (LGPL-2.1)

xiso-cartographer is MIT. It links libsmb2 statically into `default.xbe`. As the LGPL asks, the release says that the XBE contains libsmb2, carries its licence, and says where the library's source is: this folder of this repository, at the release's tag, with the port. Anyone can change libsmb2 here and build the XBE again with `make docker-xbe`.
