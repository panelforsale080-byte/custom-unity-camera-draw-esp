cmake_minimum_required(VERSION 3.22.1)
project(UnityCam)

add_library(UnityCam SHARED
    camera_app.cpp
    proc.cpp
    mem.cpp
    remote_call.cpp
    remote_il2cpp.cpp
    remote_elf.cpp
    ui_log.cpp)

target_compile_definitions(UnityCam PRIVATE GLES_SILENCE_DEPRECATION)

find_library(log-lib log)
target_link_libraries(UnityCam ${log-lib})
