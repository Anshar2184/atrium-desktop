cmake_minimum_required(VERSION 3.25)
project(atrium_native VERSION 0.9.3 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the core starts even if no runtime is installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/DUNICODE;/D_UNICODE>")

add_subdirectory(core)
