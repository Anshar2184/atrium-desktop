cmake_minimum_required(VERSION 3.25)
project(atrium_native VERSION 0.8.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: both executables start even if no runtime is installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/DUNICODE;/D_UNICODE>")

add_executable(atrium_guard guard/atrium_guard.cpp)
set_target_properties(atrium_guard PROPERTIES OUTPUT_NAME "Atrium.Guard")
target_link_libraries(atrium_guard PRIVATE user32 shell32)

add_subdirectory(core)
