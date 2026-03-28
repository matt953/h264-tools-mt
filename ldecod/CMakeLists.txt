file(GLOB LDECODSRCS "*.c")

# Library sources: everything except the test main
set(LDECOD_LIB_SRCS ${LDECODSRCS})
list(FILTER LDECOD_LIB_SRCS EXCLUDE REGEX "decoder_test\\.c$")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -pedantic -fno-strict-aliasing -fsigned-char -Wall -Wno-misleading-indentation")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -mcpu=apple-m1 -flto -Wno-error -fprofile-instr-use=/tmp/ldecod.profdata")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -flto")

# Static library (all sources except decoder_test.c)
add_library(ldecod_static STATIC ${LDECOD_LIB_SRCS})
target_link_libraries(ldecod_static m)
set_target_properties(ldecod_static PROPERTIES OUTPUT_NAME ldecod)

# Executable links against the static library
add_executable(ldecod_bin decoder_test.c)
target_link_libraries(ldecod_bin ldecod_static m)
set_target_properties(ldecod_bin PROPERTIES OUTPUT_NAME ldecod)

install(TARGETS ldecod_bin RUNTIME DESTINATION bin)
install(TARGETS ldecod_static ARCHIVE DESTINATION lib)
install(FILES decoder.cfg DESTINATION share)
install(FILES h264decoder.h global.h defines.h win32.h typedefs.h DESTINATION include/ldecod)
