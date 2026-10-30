# Unity (third_party/unity, pinned submodule) is built as-is: no project warning
# flags, and SYSTEM includes so its headers can't fail -Werror in our tests.
add_library(unity STATIC ${PROJECT_SOURCE_DIR}/third_party/unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/unity/src)
# Image offsets are 64-bit, so the 64-bit assertions must always be available.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64
  # Use the C11 keyword directly. Otherwise Unity includes <stdnoreturn.h> under clang-cl,
  # whose `noreturn` macro breaks __declspec(noreturn) in the Windows SDK headers.
  UNITY_NORETURN=_Noreturn)

# Synthetic xiso fixtures (tools/fixtures/), generated at build time.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
option(XCAT_LARGE_FIXTURES "Also generate and test the >4 GiB sparse fixture" OFF)
set(XCAT_FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
# The late* fixtures put their directory tables after their files (sequential installs).
set(XCAT_FIXTURES small deep redump-xgd1 redump-xgd2 redump-xgd3 late late-deep late-xgd3 late-root)
set(XCAT_LATE_FIXTURES late late-deep late-xgd3 late-root)
set(_fixture_args)
if(XCAT_LARGE_FIXTURES)
  list(APPEND XCAT_FIXTURES large)
  set(_fixture_args --large)
endif()
set(_fixture_outputs)
foreach(_name IN LISTS XCAT_FIXTURES)
  list(APPEND _fixture_outputs ${XCAT_FIXTURES_DIR}/${_name}.iso ${XCAT_FIXTURES_DIR}/${_name}.tsv)
endforeach()
add_custom_command(
  OUTPUT ${_fixture_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/fixtures.py
          --out ${XCAT_FIXTURES_DIR} ${_fixture_args}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fixtures/fixtures.py
          ${PROJECT_SOURCE_DIR}/tools/fixtures/xisogen.py
  COMMENT "Generating xiso test fixtures"
  VERBATIM)
add_custom_target(xcat_fixtures ALL DEPENDS ${_fixture_outputs})

# Zips of the small and late fixtures, stored and Deflate (tools/fixtures/zipgen.py).
set(_zip_outputs)
foreach(_name IN ITEMS small late)
  foreach(_kind IN ITEMS stored deflate deflate0)
    list(APPEND _zip_outputs ${XCAT_FIXTURES_DIR}/${_name}-${_kind}.zip)
  endforeach()
endforeach()
add_custom_command(
  OUTPUT ${_zip_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/zipgen.py
          --fixtures ${XCAT_FIXTURES_DIR} small late
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fixtures/zipgen.py ${XCAT_FIXTURES_DIR}/small.iso
          ${XCAT_FIXTURES_DIR}/late.iso
  COMMENT "Generating zip test fixtures"
  VERBATIM)
add_custom_target(xcat_zip_fixtures ALL DEPENDS ${_zip_outputs})
add_dependencies(xcat_zip_fixtures xcat_fixtures)

# Test code uses plain stdio (fopen); the CRT's "use fopen_s" deprecation doesn't apply.
function(xcat_test_target target)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endfunction()

# Puts LLVM's ASan DLL first on PATH for clang-cl builds; it must win over MSVC's
# identically named one.
function(xcat_test_env test)
  if(XCAT_ASAN_RUNTIME_DIR)
    set_tests_properties(${test} PROPERTIES
      ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${XCAT_ASAN_RUNTIME_DIR}")
  endif()
endfunction()

# xcat_add_test(test_foo [ARGS ...]) builds test_foo.c into an executable and
# registers it with CTest, passing ARGS on the command line.
function(xcat_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "ARGS")
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE xcat::xcat xcat::host unity)
  xcat_test_target(${name})
  # Tests may use libxcat's internal headers (e.g. bytes.h).
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
  xcat_target_warnings(${name})
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
  xcat_test_env(${name})
endfunction()

# xcat_add_fuzzer(fuzz_foo SEEDS ...) builds fuzz_foo.c twice: as fuzz_foo_replay
# (any compiler; CTest runs it over SEEDS as test fuzz_foo_seeds) and, with
# XCAT_FUZZ, as the libFuzzer binary fuzz_foo. See CONTRIBUTING.md#fuzzing.
function(xcat_add_fuzzer name)
  cmake_parse_arguments(PARSE_ARGV 1 F "" "" "SEEDS")
  add_executable(${name}_replay ${name}.c support/fuzz_replay.c)
  target_link_libraries(${name}_replay PRIVATE xcat::xcat)
  xcat_test_target(${name}_replay)
  xcat_target_warnings(${name}_replay)
  add_test(NAME ${name}_seeds COMMAND ${name}_replay ${F_SEEDS})
  xcat_test_env(${name}_seeds)
  if(XCAT_FUZZ)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE xcat::xcat)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    xcat_target_warnings(${name})
  endif()
endfunction()

xcat_add_test(test_version)
xcat_add_test(test_bytes)
xcat_add_test(test_crc32)
xcat_add_test(test_credentials)
xcat_add_test(test_map)
xcat_add_test(test_map_edit)
xcat_add_test(test_json)
xcat_add_test(test_archiveorg)
xcat_add_test(test_xbe)
xcat_add_test(test_catalog)
xcat_add_test(test_journal)
xcat_add_test(test_install ARGS ${XCAT_FIXTURES_DIR})
xcat_add_test(test_installed)
xcat_add_test(test_login)
xcat_add_test(test_settings)
xcat_add_test(test_ui_script)
add_dependencies(test_install xcat_fixtures xcat_zip_fixtures)
xcat_add_test(test_xdvdfs ARGS ${XCAT_FIXTURES_DIR} ${XCAT_FIXTURES})
add_dependencies(test_xdvdfs xcat_fixtures)
xcat_add_test(test_extract)
xcat_add_test(test_fatx)
xcat_add_test(test_url)
xcat_add_test(test_folder_image)
xcat_add_test(test_smb)
xcat_add_test(test_netbios)
xcat_add_test(test_http)
xcat_add_test(test_http_client)
# TLS against an in-process BearSSL server, with the committed test certificates.
xcat_add_test(test_sntp)
xcat_add_test(test_hash)
xcat_add_test(test_zip ARGS ${XCAT_FIXTURES_DIR})
add_dependencies(test_zip xcat_zip_fixtures)
xcat_add_test(test_zip_read ARGS ${XCAT_FIXTURES_DIR})
xcat_add_test(test_seq ARGS ${XCAT_FIXTURES_DIR})
add_dependencies(test_seq xcat_fixtures)
add_dependencies(test_zip_read xcat_zip_fixtures)
xcat_add_test(test_tls ARGS ${PROJECT_SOURCE_DIR}/tools/fixtures/tls)
target_link_libraries(test_tls PRIVATE bearssl)

# FATX installability (ADR 0008): every fixture can be installed except "deep" (and
# "late-deep" and "late-root", the same tree), whose names
# of 43 and 255 characters can't be stored on FATX; `xcat-cli check` must refuse it (exit 3).
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME check_${_name} COMMAND xcat-cli check ${XCAT_FIXTURES_DIR}/${_name}.iso)
  xcat_test_env(check_${_name})
endforeach()
set_tests_properties(check_deep check_late-deep check_late-root PROPERTIES WILL_FAIL TRUE)

# `xcat-cli layout` runs on every fixture (test_xdvdfs checks the numbers): the late* ones
# need a second pass, the others don't.
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME layout_${_name} COMMAND xcat-cli layout ${XCAT_FIXTURES_DIR}/${_name}.iso)
  if(_name IN_LIST XCAT_LATE_FIXTURES)
    set_tests_properties(layout_${_name} PROPERTIES PASS_REGULAR_EXPRESSION "one sequential pass is not enough")
  else()
    set_tests_properties(layout_${_name} PROPERTIES PASS_REGULAR_EXPRESSION "one sequential pass can install it")
  endif()
  xcat_test_env(layout_${_name})
endforeach()

# `xcat-cli zip` on the generated zips: both list and find the image.
foreach(_kind IN ITEMS stored deflate)
  add_test(NAME zip_cli_${_kind} COMMAND xcat-cli zip ${XCAT_FIXTURES_DIR}/small-${_kind}.zip)
  set_tests_properties(zip_cli_${_kind} PROPERTIES PASS_REGULAR_EXPRESSION "image: small.iso")
  xcat_test_env(zip_cli_${_kind})
endforeach()
add_test(NAME zip_cli_not_zip COMMAND xcat-cli zip ${XCAT_FIXTURES_DIR}/small.iso)
set_tests_properties(zip_cli_not_zip PROPERTIES WILL_FAIL TRUE)
xcat_test_env(zip_cli_not_zip)

# End to end: xcat-cli extracts every fixture, and the result must match its manifest (and so
# extract-xiso's output: tools/fixtures/verify.sh checks the manifests against extract-xiso).
foreach(_name IN LISTS XCAT_FIXTURES)
  add_test(NAME extract_${_name}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/extract_and_check.py
            --cli $<TARGET_FILE:xcat-cli> --fixtures ${XCAT_FIXTURES_DIR}
            --out ${CMAKE_BINARY_DIR}/extracted ${_name})
  xcat_test_env(extract_${_name})
endforeach()

# End to end over the network: `xcat-cli get` against the Range test server, one test per
# scenario (normal downloads, redirects, drops, stalls, error statuses; tools/testserver/).
execute_process(
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_get.py --list
  OUTPUT_VARIABLE _get_scenarios OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/tools/testserver/test_get.py)
foreach(_name IN LISTS _get_scenarios)
  add_test(NAME get_${_name}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_get.py
            --cli $<TARGET_FILE:xcat-cli> --work ${CMAKE_BINARY_DIR}/get-tests ${_name})
  xcat_test_env(get_${_name})
endforeach()

# `xcat-cli install` end to end: fixtures over HTTP, resume after a kill, verification
# (tools/testserver/test_install.py).
execute_process(
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_install.py --list
  OUTPUT_VARIABLE _install_scenarios OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/tools/testserver/test_install.py)
foreach(_name IN LISTS _install_scenarios)
  add_test(NAME install_${_name}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_install.py
            --cli $<TARGET_FILE:xcat-cli> --fixtures ${XCAT_FIXTURES_DIR}
            --work ${CMAKE_BINARY_DIR}/install-tests ${_name})
  xcat_test_env(install_${_name})
endforeach()

# `xcat-cli ntp` end to end against a fake SNTP server, one test per misbehaviour
# (tools/testserver/test_ntp.py, docs/formats/sntp.md §4 and §6).
execute_process(
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_ntp.py --list
  OUTPUT_VARIABLE _ntp_scenarios OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/tools/testserver/test_ntp.py)
foreach(_name IN LISTS _ntp_scenarios)
  add_test(NAME ntp_${_name}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_ntp.py
            --cli $<TARGET_FILE:xcat-cli> ${_name})
  xcat_test_env(ntp_${_name})
endforeach()

# `xcat-cli map` end to end: local and HTTP includes, problems and the exit status.
add_test(NAME map_cli
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/testserver/test_map_cli.py
          --cli $<TARGET_FILE:xcat-cli> --work ${CMAKE_BINARY_DIR}/get-tests)
xcat_test_env(map_cli)

# Fuzz seeds: the directory tables of the xiso fixtures, in the harness's input layout.
set(XCAT_FUZZ_SEEDS_DIR ${CMAKE_BINARY_DIR}/fuzz-seeds)
set(_seed_fixtures small deep)
set(_seed_outputs)
set(_seed_inputs)
foreach(_name IN LISTS _seed_fixtures)
  list(APPEND _seed_outputs ${XCAT_FUZZ_SEEDS_DIR}/${_name}.seed)
  list(APPEND _seed_inputs ${XCAT_FIXTURES_DIR}/${_name}.iso)
endforeach()
add_custom_command(
  OUTPUT ${_seed_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/fuzz_seeds.py
          --fixtures ${XCAT_FIXTURES_DIR} --out ${XCAT_FUZZ_SEEDS_DIR} ${_seed_fixtures}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/fixtures/fuzz_seeds.py ${_seed_inputs}
  COMMENT "Generating fuzz seeds"
  VERBATIM)
add_custom_target(xcat_fuzz_seeds ALL DEPENDS ${_seed_outputs})

xcat_add_fuzzer(fuzz_xdvdfs SEEDS ${_seed_outputs})
add_dependencies(fuzz_xdvdfs_replay xcat_fuzz_seeds)

# HTTP parser and URL seeds are committed (exact bytes; see .gitattributes).
file(GLOB _http_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/http/*.seed)
xcat_add_fuzzer(fuzz_http SEEDS ${_http_seeds})

# Credentials file seeds are committed too.
file(GLOB _cred_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/credentials/*.seed)
xcat_add_fuzzer(fuzz_credentials SEEDS ${_cred_seeds})

# Map seeds are committed: NUL-separated parts, see fuzz_map.c.
file(GLOB _map_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/map/*.seed)
xcat_add_fuzzer(fuzz_map SEEDS ${_map_seeds})

# Map editing seeds are committed: synthetic maps and typed lines.
file(GLOB _map_edit_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/map-edit/*.seed)
xcat_add_fuzzer(fuzz_map_edit SEEDS ${_map_edit_seeds})

# archive.org metadata seeds are committed: synthetic listings with placeholder names.
file(GLOB _archiveorg_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/archiveorg/*.seed)
xcat_add_fuzzer(fuzz_archiveorg SEEDS ${_archiveorg_seeds})

# XBE seeds are committed: synthetic headers with placeholder titles.
file(GLOB _xbe_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/xbe/*.seed)
xcat_add_fuzzer(fuzz_xbe SEEDS ${_xbe_seeds})

# Install marker seeds are committed: synthetic markers with placeholder names.
file(GLOB _installed_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/installed/*.seed)
xcat_add_fuzzer(fuzz_installed SEEDS ${_installed_seeds})

# Login seeds are committed: synthetic answers and credentials files with placeholder values.
file(GLOB _login_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/login/*.seed)
xcat_add_fuzzer(fuzz_login SEEDS ${_login_seeds})

# Settings seeds are committed: synthetic settings files and folders.
file(GLOB _settings_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/settings/*.seed)
xcat_add_fuzzer(fuzz_settings SEEDS ${_settings_seeds})

# UI script seeds are committed, plus the walkthrough itself (docs/formats/ui-script.md).
file(GLOB _ui_script_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/ui-script/*.seed)
xcat_add_fuzzer(fuzz_ui_script SEEDS ${_ui_script_seeds}
                ${PROJECT_SOURCE_DIR}/tools/ui/walkthrough.txt)

# Catalog seeds are committed, written by tools/fixtures/catalog_seeds.py with the pipeline's
# writer (tools/catalog/catalog_bin.py), so the reader is checked against it on every build.
set(_catalog_seed_dir ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/catalog)
file(GLOB _catalog_seeds ${_catalog_seed_dir}/*.seed)
xcat_add_fuzzer(fuzz_catalog SEEDS ${_catalog_seeds})

# `xcat-cli catalog` on the seeds: list, search, look up by title key and by name, reject.
# catalog_cli(<name> <seed> <expected output regex, or FAIL> [args...])
function(catalog_cli name seed expect)
  add_test(NAME catalog_cli_${name}
    COMMAND xcat-cli catalog ${_catalog_seed_dir}/${seed}.seed ${ARGN})
  if(expect STREQUAL "FAIL")
    set_tests_properties(catalog_cli_${name} PROPERTIES WILL_FAIL TRUE)
  else()
    set_tests_properties(catalog_cli_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
  endif()
  xcat_test_env(catalog_cli_${name})
endfunction()
catalog_cli(list good "4 releases.*Charlie Quest.*3 games")
catalog_cli(query good "Charlie Quest.*1 game\n" "studio three")
catalog_cli(title good "41410001@EU .*41410001@US " --title 41410001)
catalog_cli(variant good "Alpha Racer \\(Europe\\)" --title 41410001@eu)
catalog_cli(name good "43430010@US-EU.*region: USA, Europe" --name "charlie quest (usa, europe)")
catalog_cli(newer newer-minor "version 1.1, 3 games")
catalog_cli(not_found good FAIL --name "Alpha Racer")
catalog_cli(rejected names-unsorted FAIL)
catalog_cli(major major-2 FAIL)

# The catalog pipeline (tools/catalog/) on a synthetic cache: joins, variants, overrides,
# the lock, a deterministic rebuild, and its output opened by xcat-cli. Needs Python 3.11.
add_test(NAME catalog_pipeline
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/catalog/test_pipeline.py
          --cli $<TARGET_FILE:xcat-cli> --work ${CMAKE_BINARY_DIR}/catalog-tests)
xcat_test_env(catalog_pipeline)

# The release zip (tools/release/package.py, docs/release.md) from a made-up repo and app/bin:
# its contents and licences, the same bytes every time, the version check.
add_test(NAME release_package
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/release/test_package.py)
xcat_test_env(release_package)

# Journal seeds are committed, without their line CRCs (fuzz_journal.c adds them).
file(GLOB _journal_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/journal/*.seed)
xcat_add_fuzzer(fuzz_journal SEEDS ${_journal_seeds})

# Trust-anchor (PEM bundle) seeds are committed: the test certificates and a few broken ones.
file(GLOB _tls_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/tls/*.seed)
xcat_add_fuzzer(fuzz_tls_anchors SEEDS ${_tls_seeds})

# SNTP answer seeds are committed, written by tools/fixtures/sntp_seeds.py.
file(GLOB _sntp_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/sntp/*.seed)
xcat_add_fuzzer(fuzz_sntp SEEDS ${_sntp_seeds})

# smb:// URL seeds are committed (docs/formats/smb.md §1).
file(GLOB _smb_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/smb/*.seed)
xcat_add_fuzzer(fuzz_smb SEEDS ${_smb_seeds})

# Game folder listing seeds are committed (docs/formats/folder-image.md).
file(GLOB _folder_image_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/folder-image/*.seed)
xcat_add_fuzzer(fuzz_folder_image SEEDS ${_folder_image_seeds})

# NetBIOS node status answer seeds are committed: synthetic answers as Samba and Windows send
# them (docs/formats/smb.md §5).
file(GLOB _netbios_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/netbios/*.seed)
xcat_add_fuzzer(fuzz_netbios SEEDS ${_netbios_seeds})

# Zip and Deflate seeds are committed, written by tools/fixtures/zip_seeds.py.
file(GLOB _zip_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/zip/*.seed)
xcat_add_fuzzer(fuzz_zip SEEDS ${_zip_seeds})
file(GLOB _zip_inflate_seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz-seeds/zip-inflate/*.seed)
xcat_add_fuzzer(fuzz_zip_inflate SEEDS ${_zip_inflate_seeds})

# Sequential extraction seeds: whole fixture images (tables first, tables last, root last),
# copied from the generated fixtures (outside fuzz-seeds/, which fuzz_xdvdfs reads whole).
set(_seq_seeds)
foreach(_name IN ITEMS small late late-root)
  set(_seed ${CMAKE_BINARY_DIR}/fuzz-seeds-seq/${_name}.seed)
  add_custom_command(
    OUTPUT ${_seed}
    COMMAND ${CMAKE_COMMAND} -E copy ${XCAT_FIXTURES_DIR}/${_name}.iso ${_seed}
    DEPENDS ${XCAT_FIXTURES_DIR}/${_name}.iso
    VERBATIM)
  list(APPEND _seq_seeds ${_seed})
endforeach()
# ... and with a power cut and a resume (stream-install.md §7): <image> <cut> <disk>
foreach(_v IN ITEMS "late;0x40;0" "late;0x21;0x40" "late-root;0x52;0x40" "late;0xC8;0")
  list(GET _v 0 _name)
  list(GET _v 1 _cut)
  list(GET _v 2 _disk)
  set(_seed ${CMAKE_BINARY_DIR}/fuzz-seeds-seq/${_name}-cut-${_cut}-${_disk}.seed)
  add_custom_command(
    OUTPUT ${_seed}
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/fixtures/seq_seed.py
            ${XCAT_FIXTURES_DIR}/${_name}.iso ${_seed} ${_cut} ${_disk}
    DEPENDS ${XCAT_FIXTURES_DIR}/${_name}.iso ${PROJECT_SOURCE_DIR}/tools/fixtures/seq_seed.py
    VERBATIM)
  list(APPEND _seq_seeds ${_seed})
endforeach()
add_custom_target(xcat_seq_seeds ALL DEPENDS ${_seq_seeds})
add_dependencies(xcat_seq_seeds xcat_fixtures)
xcat_add_fuzzer(fuzz_seq SEEDS ${_seq_seeds})
add_dependencies(fuzz_seq_replay xcat_seq_seeds)
