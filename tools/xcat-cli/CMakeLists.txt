# xcat-cli: runs libxcat on a PC (list and extract images).
add_executable(xcat-cli main.c)
target_link_libraries(xcat-cli PRIVATE xcat::xcat xcat::host)
xcat_target_warnings(xcat-cli)
if(NOT WIN32)
  # gmtime_r (xcat-cli ntp) isn't declared under strict C11 without this.
  target_compile_definitions(xcat-cli PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(XCAT_SMB)
  # SMB sources (docs/formats/smb.md): smb-scan, smb-shares, smb-ls, smb-hash, and smb:// in map
  # and install.
  target_sources(xcat-cli PRIVATE smb_cmds.c)
  target_link_libraries(xcat-cli PRIVATE xcat::smb)
  target_compile_definitions(xcat-cli PRIVATE XCAT_SMB)
  set_source_files_properties(smb_cmds.c PROPERTIES COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)
endif()
