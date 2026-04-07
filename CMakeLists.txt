cmake_minimum_required(VERSION 3.20)
project(sim_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(SDL2 REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
    libswscale
)

add_executable(sim_player
    src/main.cpp
    src/Demuxer.cpp
    src/Decoder.cpp
    src/Player.cpp
    src/RendererSDL.cpp
)

target_include_directories(sim_player
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sim_player
    PRIVATE
        PkgConfig::FFMPEG
        SDL2::SDL2
)

if (TARGET SDL2::SDL2main)
    target_link_libraries(sim_player PRIVATE SDL2::SDL2main)
endif ()
