# libxcat: portable core. Must not depend on nxdk or any platform headers.
include(${PROJECT_SOURCE_DIR}/cmake/BearSSL.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/miniz.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/lzma.cmake)

add_library(xcat
  src/archiveorg.c
  src/catalog.c
  src/crc32.c
  src/credentials.c
  src/error.c
  src/extract.c
  src/fatx.c
  src/hash.c
  src/http.c
  src/arc_folder.c
  src/folder_image.c
  src/http_client.c
  src/install.c
  src/installed.c
  src/journal.c
  src/json.c
  src/login.c
  src/settings.c
  src/sevenz.c
  src/smb.c
  src/map.c
  src/map_edit.c
  src/md4.c
  src/netbios.c
  src/seq.c
  src/source.c
  src/sntp.c
  src/text.c
  src/tls.c
  src/ui_script.c
  src/url.c
  src/version.c
  src/xbe.c
  src/xdvdfs.c
  src/zip.c
  src/zip_read.c
  src/zip_folder.c
)
add_library(xcat::xcat ALIAS xcat)

# The built-in CA bundle (ADR 0012 §3), embedded from the pinned third_party/cacert/cacert.pem.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(_cacert ${PROJECT_SOURCE_DIR}/third_party/cacert/cacert.pem)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cacert_data.c
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/cacert/embed_cacert.py
          --in ${_cacert} --out ${CMAKE_CURRENT_BINARY_DIR}/cacert_data.c
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/cacert/embed_cacert.py ${_cacert} ${_cacert}.sha256
  COMMENT "Embedding the CA bundle"
  VERBATIM)
target_sources(xcat PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cacert_data.c)
target_include_directories(xcat PRIVATE src)

target_include_directories(xcat PUBLIC include)
target_compile_features(xcat PUBLIC c_std_11)
xcat_target_warnings(xcat)
# Only tls.c and hash.c use BearSSL (ADR 0005).
target_link_libraries(xcat PRIVATE bearssl miniz_tinfl lzma_sdk)
