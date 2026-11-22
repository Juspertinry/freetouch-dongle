cmake_minimum_required(VERSION 3.16)
project(freetouch C)

# The host side: libfreetouch (a static library), freetouch-cli and the tests. The firmware has its
# own build (firmware/build.py), which needs the Arm GNU toolchain.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(freetouch STATIC host/src/stream.c host/src/port.c)
target_include_directories(freetouch PUBLIC host/include)
if(WIN32)
	target_link_libraries(freetouch PUBLIC setupapi advapi32)
endif()

add_executable(freetouch-cli tools/freetouch-cli.c)
target_link_libraries(freetouch-cli PRIVATE freetouch)

enable_testing()
add_executable(test_freetouch tests/test_freetouch.c)
target_link_libraries(test_freetouch PRIVATE freetouch)
add_test(NAME libfreetouch COMMAND test_freetouch)

install(TARGETS freetouch freetouch-cli)
install(DIRECTORY host/include/freetouch DESTINATION include)
