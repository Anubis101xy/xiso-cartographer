# stb_image (JPEG and PNG decoder)

Sean Barrett's single-file image loader, from <https://github.com/nothings/stb>: `stb_image.h` **v2.30** as it is at commit `d14d46f` (2026-11-01), unchanged, with the repository's `LICENSE` (public domain, or MIT, at your choice).

xiso-cartographer uses it to decode the cover pictures a NAS may hold beside a game (`poster.jpg`, `default.tbn`, `cover.png`; [docs/formats/art.md](../../docs/formats/art.md), ADR 0019). `core/src/stb_image_impl.c` compiles it with only its JPEG and PNG decoders (`STBI_ONLY_JPEG`, `STBI_ONLY_PNG`), no file I/O, no HDR or linear-light conversion, no SIMD (the console's Pentium III has no SSE2), with `-fwrapv` (a crafted JPEG overflows the IDCT's `int` arithmetic, which must then wrap; `fuzz_picture` found it), and the project's own allocator, which caps what one picture may allocate; `core/src/picture.c` refuses a picture over 2048 pixels a side or 1536 × 1536 in all from its header, before it's decoded. The other formats it can read (GIF, BMP, TGA, PSD, PIC, PNM, HDR), where its open security reports are, aren't compiled. Its `assert`s are on in the PC builds (tests and fuzzers) and off in the XBE, where a picture that trips one is refused instead of stopping the app. In the sanitizer builds its `memcpy(NULL, …, 0)` for an empty PNG `IDAT` (copying nothing) isn't reported (`-fno-sanitize=nonnull-attribute`, `cmake/stb.cmake`). `core/tests/fuzz-seeds/picture/` keeps the inputs that found these two.

## Updating

Copy `stb_image.h` and `LICENSE` from a newer commit, check the licence still says public domain or MIT, run the fuzzer (`fuzz_picture`), and update the version and commit here and in `THIRD_PARTY.md`.
