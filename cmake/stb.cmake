# stb_image's JPEG and PNG decoders (third_party/stb, public domain or MIT; ADR 0019,
# docs/formats/art.md), built from core/src/stb_image_impl.c with its own warnings off, as for
# miniz and the LZMA SDK. Its asserts stay on here (tests, fuzzers); app/art.mk turns them off.
add_library(stb_image STATIC ${PROJECT_SOURCE_DIR}/core/src/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
target_compile_definitions(stb_image PRIVATE XCAT_STB_ASSERT)
target_compile_options(stb_image PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
# A crafted JPEG overflows stb's int arithmetic in its IDCT (found by fuzz_picture): -fwrapv
# makes that defined (it wraps, and the picture comes out wrong, not the program), as other
# fuzzed users of stb_image build it.
if(NOT MSVC)
  target_compile_options(stb_image PRIVATE -fwrapv)
endif()
# A PNG with an empty IDAT before any data makes stb call memcpy(NULL, ..., 0) (found by
# fuzz_picture): copying nothing, which every C library does safely, but which the sanitizer's
# nonnull check reports. Like the LZMA SDK's alignment exemption, it's not for this code.
if(XCAT_SANITIZE AND NOT MSVC)
  target_compile_options(stb_image PRIVATE -fno-sanitize=nonnull-attribute)
endif()
