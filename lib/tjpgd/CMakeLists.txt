# TJpgDec R0.03 (C)ChaN, 2021 - http://elm-chan.org/fsw/tjpgd/
# Local config changes: JD_SZBUF 2048, JD_FASTDECODE 2 (see tjpgdcnf.h)
add_library(tjpgd tjpgd.c)
target_include_directories(tjpgd PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(tjpgd PRIVATE -O2)
