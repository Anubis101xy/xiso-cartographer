# LZMA SDK (7z decoder subset)

The 7z archive reader and the LZMA, LZMA2, BCJ (x86) and BCJ2 decoders from Igor Pavlov's LZMA SDK, as published in 7-Zip's source: <https://github.com/ip7z/7zip>, tag **25.01** (commit `5e96a82`), folder `C/`. Each file states in its header that it's in the **public domain**, and `7zC.txt` (from the same tag's `DOC/`) says the same of the SDK as a whole.

xiso-cartographer uses it to install games and apps from `.7z` archives ([docs/formats/sevenz.md](../../docs/formats/sevenz.md), ADR 0018 decision 9): `7zArcIn.c` reads the archive's database (and `7zDec.c` decodes it when it's compressed), and `core/src/sevenz.c` streams each folder's data through `LzmaDec.c`, `Lzma2Dec.c` and `Bra86.c` itself, so a large image never has to fit in memory.

## What's here

`C/`: the files below, unchanged from the tag:

`7z.h`, `7zTypes.h`, `7zArcIn.c`, `7zBuf.c`, `7zBuf.h`, `7zCrc.h`, `7zDec.c`, `7zStream.c`, `7zVersion.h`, `7zWindows.h`, `Bcj2.c`, `Bcj2.h`, `Bra.h`, `Bra86.c`, `Compiler.h`, `CpuArch.h`, `Delta.h` (7zDec.c includes it; its filter isn't built), `LzmaDec.c`, `LzmaDec.h`, `Lzma2Dec.c`, `Lzma2Dec.h`, `Precomp.h`, `RotateDefs.h`.

Not taken: `7zCrc.c`, `7zCrcOpt.c` and `CpuArch.c` (CPU feature detection for hardware CRC). `core/src/sevenz.c` defines `CrcGenerateTable`, `CrcUpdate` and `CrcCalc` over the project's own CRC-32 (`xcat/crc32.h`, the same polynomial) instead.

## Building

Compiled with `Z7_NO_METHODS_FILTERS` (7zDec.c then decodes only what archive headers use: copy, LZMA, LZMA2, BCJ2) and without `Z7_PPMD_SUPPORT`, at `-O2` with the SDK's own warnings off: `cmake/lzma.cmake` on the PC, `app/sevenz.mk` for the XBE.

## Updating

Copy the same files from a newer tag, check each still says "Public domain", and update the tag and commit here and in `THIRD_PARTY.md`.
