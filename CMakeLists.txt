cmake_minimum_required(VERSION 3.16)
project(apctl-hotspot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v7.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

add_executable(apctl
    src/cli/main.cpp
    src/core/hotspot.cpp
    src/core/process.cpp
    src/tui/tui.cpp
)
target_link_libraries(apctl PRIVATE ftxui::component ftxui::dom ftxui::screen)
target_compile_options(apctl PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS apctl RUNTIME DESTINATION bin)
