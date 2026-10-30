# libsmb2 (third_party/libsmb2, vendored with the nxdk port: third_party/libsmb2/xcat/README.md;
# ADR 0018), built as-is for the host: no project warning flags, SYSTEM includes so its headers
# can't fail -Werror, and a config.h from the same checks its own CMake makes.
#
# Host builds use it for the SMB client (platform/smb) and xcat-cli's smb commands. It's built on
# Linux and macOS; Windows hosts build without SMB (XCAT_SMB off), since the XBE is the target
# and libsmb2's Winsock code isn't ours to keep working there.

if(WIN32)
  set(XCAT_SMB OFF)
  return()
endif()
set(XCAT_SMB ON)

set(_smb2_dir ${PROJECT_SOURCE_DIR}/third_party/libsmb2)
if(NOT EXISTS ${_smb2_dir}/include/smb2/libsmb2.h)
  message(FATAL_ERROR "third_party/libsmb2 is missing")
endif()

include(CheckIncludeFile)
include(CheckIncludeFiles)
include(CheckStructHasMember)
include(CheckSymbolExists)
foreach(_h arpa/inet.h dlfcn.h fcntl.h inttypes.h netdb.h netinet/in.h poll.h stdint.h stdio.h
        stdlib.h strings.h string.h sys/ioctl.h sys/poll.h sys/socket.h sys/stat.h sys/types.h
        sys/uio.h sys/time.h sys/unistd.h sys/errno.h time.h unistd.h errno.h)
  string(TOUPPER "HAVE_${_h}" _var)
  string(REGEX REPLACE "[/.]" "_" _var "${_var}")
  check_include_file("${_h}" ${_var})
endforeach()
check_include_files("sys/types.h;netinet/tcp.h" HAVE_NETINET_TCP_H)
check_include_file("stddef.h" STDC_HEADERS)
check_struct_has_member("struct sockaddr" sa_len sys/socket.h HAVE_SOCKADDR_LEN)
check_struct_has_member("struct sockaddr_storage" ss_family sys/socket.h HAVE_SOCKADDR_STORAGE)
check_struct_has_member("struct linger" l_linger sys/socket.h HAVE_LINGER)
check_symbol_exists(arc4random_buf "stdlib.h" HAVE_ARC4RANDOM_BUF)
check_symbol_exists(getrandom "sys/random.h" HAVE_GETRANDOM)

set(_smb2_gen ${CMAKE_BINARY_DIR}/libsmb2)
configure_file(${_smb2_dir}/cmake/config.h.cmake ${_smb2_gen}/config.h)

set(_smb2_sources
  aes.c aes_reference.c aes128ccm.c alloc.c asn1-ber.c compat.c errors.c hmac.c hmac-md5.c
  init.c libsmb2.c md4c.c md5.c ntlmssp.c pdu.c quic.c smbdirect.c sha1.c sha224-256.c
  sha384-512.c smb2-cmd-cancel.c smb2-cmd-close.c smb2-cmd-create.c smb2-cmd-echo.c
  smb2-cmd-error.c smb2-cmd-flush.c smb2-cmd-ioctl.c smb2-cmd-lock.c smb2-cmd-logoff.c
  smb2-cmd-negotiate.c smb2-cmd-notify-change.c smb2-cmd-oplock-break.c
  smb2-cmd-query-directory.c smb2-cmd-query-info.c smb2-cmd-read.c smb2-cmd-session-setup.c
  smb2-cmd-set-info.c smb2-cmd-tree-connect.c smb2-cmd-tree-disconnect.c smb2-cmd-write.c
  smb2-data-file-info.c smb2-data-filesystem-info.c smb2-data-security-descriptor.c
  smb2-data-reparse-point.c smb2-share-enum.c smb3-seal.c smb2-signing.c socket.c
  spnego-wrapper.c sync.c timestamps.c unicode.c usha.c)
list(TRANSFORM _smb2_sources PREPEND ${_smb2_dir}/lib/)

add_library(smb2 STATIC ${_smb2_sources})
target_include_directories(smb2 SYSTEM PUBLIC ${_smb2_dir}/include)
target_include_directories(smb2 PRIVATE ${_smb2_gen} ${_smb2_dir}/include/smb2 ${_smb2_dir}/lib)
target_compile_definitions(smb2 PRIVATE HAVE_CONFIG_H _FILE_OFFSET_BITS=64 _GNU_SOURCE
  "_U_=__attribute__((unused))")
if(HAVE_ARC4RANDOM_BUF)
  target_compile_definitions(smb2 PRIVATE HAVE_ARC4RANDOM_BUF)
endif()
if(HAVE_GETRANDOM)
  target_compile_definitions(smb2 PRIVATE HAVE_SYS_RANDOM_H HAVE_GETRANDOM)
endif()
if(EXISTS "/dev/urandom")
  target_compile_definitions(smb2 PRIVATE HAVE_DEV_URANDOM)
endif()
# libsmb2's own warnings aren't ours to fix; keep the build log readable.
target_compile_options(smb2 PRIVATE -w)
if(XCAT_SANITIZE AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
  # Like BearSSL: callbacks are called through pointers of a generic type.
  target_compile_options(smb2 PRIVATE -fno-sanitize=function)
endif()
