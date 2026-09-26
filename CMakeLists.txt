cmake_minimum_required(VERSION 3.20)
project(pmux
  VERSION 0.1.0
  DESCRIPTION "Terminal process multiplexer with a list TUI and byte-exact attach"
  HOMEPAGE_URL "https://github.com/knucklehead96/pmux"
  LANGUAGES CXX)

option(PMUX_STATIC "Build a fully static pmux executable" OFF)
option(PMUX_STRIP "Strip the pmux executable at link time" OFF)

# Single-config generators: default to an optimized (Release) build.
get_property(PMUX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT PMUX_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release: put every function/object in its own section so the linker can drop unused
# ones (applies to FTXUI too, which is added below).
add_compile_options("$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
add_link_options("$<$<CONFIG:Release>:LINKER:--gc-sections>")

include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v6.1.9
  GIT_SHALLOW TRUE
)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "pmux installs only its binary" FORCE)
FetchContent_MakeAvailable(ftxui)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)

add_executable(pmux
  src/main.cpp
  src/common/protocol.cpp
  src/common/paths.cpp
  src/common/config.cpp
  src/daemon/server.cpp
  src/daemon/session.cpp
  src/daemon/screen.cpp
  src/client/connect.cpp
  src/client/tui.cpp
  src/client/attach.cpp
)
target_include_directories(pmux PRIVATE src)
target_compile_options(pmux PRIVATE -Wall -Wextra)
target_compile_definitions(pmux PRIVATE PMUX_VERSION="${PROJECT_VERSION}")
target_link_libraries(pmux PRIVATE ftxui::screen ftxui::dom ftxui::component)

if(PMUX_STATIC)
  # Fully static: libvterm from its static archive, libstdc++/libgcc/glibc static.
  find_library(VTERM_STATIC_LIBRARY NAMES libvterm.a HINTS ${VTERM_LIBRARY_DIRS} REQUIRED)
  target_include_directories(pmux PRIVATE ${VTERM_INCLUDE_DIRS})
  target_link_libraries(pmux PRIVATE ${VTERM_STATIC_LIBRARY} util)
  target_link_options(pmux PRIVATE -static)
else()
  target_link_libraries(pmux PRIVATE PkgConfig::VTERM util)
endif()

if(PMUX_STRIP)
  target_link_options(pmux PRIVATE -s)
endif()

install(TARGETS pmux RUNTIME DESTINATION bin)
