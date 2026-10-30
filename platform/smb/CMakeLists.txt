# The SMB client and NAS scan (docs/formats/smb.md, ADR 0018) for PCs: libsmb2 under a small
# interface, used by xcat-cli. The XBE builds the same sources (app/smb.mk).
include(${PROJECT_SOURCE_DIR}/cmake/libsmb2.cmake)
if(NOT XCAT_SMB)
  return()
endif()
add_library(xcat_smb STATIC
  src/smb_client.c
  src/smb_scan.c
)
add_library(xcat::smb ALIAS xcat_smb)
target_include_directories(xcat_smb PUBLIC include)
target_link_libraries(xcat_smb PUBLIC xcat::xcat PRIVATE smb2)
xcat_target_warnings(xcat_smb)
set(XCAT_SMB ON PARENT_SCOPE)
