cmake_minimum_required(VERSION 3.22.1)
project(sf2live LANGUAGES CXX)

find_package(oboe REQUIRED CONFIG)

add_library(sf2live SHARED
    native-lib.cpp
    AudioEngine.cpp
    Sf2Synth.cpp
)

target_include_directories(sf2live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(sf2live PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(sf2live
    oboe::oboe
    amidi
    android
    log
    atomic
)
