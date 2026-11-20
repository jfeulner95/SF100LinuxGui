cmake_minimum_required(VERSION 3.20)
project(sf100 C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # gnu99

# The engine is the unmodified-layout fork of SF100Linux (git submodule).
set(ENGINE_DIR ${CMAKE_SOURCE_DIR}/external/SF100Linux)
if(NOT EXISTS ${ENGINE_DIR}/dpcmd.c)
    message(FATAL_ERROR "Engine sources missing. Run: git submodule update --init")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(LibXml2 REQUIRED)
find_package(Threads REQUIRED)

enable_testing()

# Same list as SRCS in the engine's Makefile.
set(ENGINE_SOURCES
    ${ENGINE_DIR}/dpcmd.c
    ${ENGINE_DIR}/usbdriver.c
    ${ENGINE_DIR}/FlashCommand.c
    ${ENGINE_DIR}/SerialFlash.c
    ${ENGINE_DIR}/parse.c
    ${ENGINE_DIR}/board.c
    ${ENGINE_DIR}/project.c
    ${ENGINE_DIR}/IntelHexFile.c
    ${ENGINE_DIR}/MotorolaFile.c
)

# Engine sources only: route their stdio output through sflog.c (see src/core/sf_hook.h) and give
# the chip DB lookup a last-resort install location (FindChipDbFile in the engine's parse.c).
foreach(src IN LISTS ENGINE_SOURCES)
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_OPTIONS
        "SHELL:-include \"${CMAKE_SOURCE_DIR}/src/core/sf_hook.h\"")
    set_property(SOURCE ${src} APPEND PROPERTY COMPILE_DEFINITIONS
        "SF_DATADIR=\"${CMAKE_INSTALL_FULL_DATADIR}/DediProg\"")
endforeach()
# dpcmd.c also holds the engine's global state, so it goes into the library; its main() becomes dpcmd_main().
set_property(SOURCE ${ENGINE_DIR}/dpcmd.c APPEND PROPERTY COMPILE_DEFINITIONS main=dpcmd_main)

add_library(sf100core STATIC
    ${ENGINE_SOURCES}
    src/core/sf100core.c
    src/core/sflog.c
)
target_include_directories(sf100core PUBLIC src/core PRIVATE ${ENGINE_DIR})
target_link_libraries(sf100core PUBLIC PkgConfig::LIBUSB LibXml2::LibXml2 Threads::Threads)
target_compile_options(sf100core PRIVATE -Wall)

add_executable(dpcmd src/cli/main.c)
target_link_libraries(dpcmd PRIVATE sf100core)
target_compile_options(dpcmd PRIVATE -Wall)

# Hardware-free tests of the public API (src/core/sf100core.h)
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core PRIVATE sf100core)
target_compile_options(test_core PRIVATE -Wall)
target_compile_definitions(test_core PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
add_test(NAME core COMMAND test_core)
set_tests_properties(core PROPERTIES ENVIRONMENT "DPCMD_CHIPDB=${ENGINE_DIR}/ChipInfoDb.dedicfg")

# Let the build-tree binary find its chip database next to itself.
add_custom_command(TARGET dpcmd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ENGINE_DIR}/ChipInfoDb.dedicfg $<TARGET_FILE_DIR:dpcmd>)

install(TARGETS dpcmd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${ENGINE_DIR}/ChipInfoDb.dedicfg DESTINATION ${CMAKE_INSTALL_DATADIR}/DediProg)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Absolute destination: still honours DESTDIR, so packagers can stage it elsewhere.
    install(FILES ${ENGINE_DIR}/60-dediprog.rules DESTINATION /etc/udev/rules.d)
endif()
