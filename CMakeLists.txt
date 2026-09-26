cmake_minimum_required(VERSION 3.20)
project(pmux CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG v6.1.9
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

find_package(PkgConfig REQUIRED)
pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)

add_executable(pmux
  src/main.cpp
  src/common/protocol.cpp
  src/common/paths.cpp
  src/daemon/server.cpp
  src/daemon/session.cpp
  src/daemon/screen.cpp
  src/client/connect.cpp
  src/client/tui.cpp
  src/client/attach.cpp
)
target_include_directories(pmux PRIVATE src)
target_compile_options(pmux PRIVATE -Wall -Wextra)
target_link_libraries(pmux PRIVATE
  ftxui::screen ftxui::dom ftxui::component
  PkgConfig::VTERM
  util
)
