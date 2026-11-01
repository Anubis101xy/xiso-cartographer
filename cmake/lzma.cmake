# The LZMA SDK's 7z decoder subset (third_party/lzma, public domain; ADR 0018 decision 9,
# docs/formats/sevenz.md): the archive reader, LZMA, LZMA2, BCJ and BCJ2. Built with its own
# warnings off, as for miniz; core/src/sevenz.c supplies its CRC-32 (third_party/lzma/README.md).
set(_lz_dir ${PROJECT_SOURCE_DIR}/third_party/lzma/C)
add_library(lzma_sdk STATIC
  ${_lz_dir}/7zArcIn.c
  ${_lz_dir}/7zBuf.c
  ${_lz_dir}/7zDec.c
  ${_lz_dir}/7zStream.c
  ${_lz_dir}/Bcj2.c
  ${_lz_dir}/Bra86.c
  ${_lz_dir}/LzmaDec.c
  ${_lz_dir}/Lzma2Dec.c
)
target_include_directories(lzma_sdk SYSTEM PUBLIC ${_lz_dir})
target_compile_definitions(lzma_sdk PUBLIC Z7_NO_METHODS_FILTERS)
target_compile_options(lzma_sdk PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
# On x86 the SDK reads little-endian integers with unaligned loads on purpose (CpuArch.h,
# MY_CPU_LE_UNALIGN), which the CPU allows; the sanitizer's alignment check isn't for it.
if(XCAT_SANITIZE AND NOT MSVC)
  target_compile_options(lzma_sdk PRIVATE -fno-sanitize=alignment)
endif()
