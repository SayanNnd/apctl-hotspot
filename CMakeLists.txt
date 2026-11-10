cmake_minimum_required(VERSION 3.16)
project(apctl-hotspot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(apctl src/main.cpp)
target_compile_options(apctl PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS apctl RUNTIME DESTINATION bin)
