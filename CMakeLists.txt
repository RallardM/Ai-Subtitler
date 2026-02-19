cmake_minimum_required(VERSION 3.16)

project(AiSubtitler LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Keep the submodule lean in this parent build.
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)

add_subdirectory(submodules/whisper.cpp)

# SDL2 (required for microphone capture)
find_package(SDL2 CONFIG QUIET)
if (SDL2_FOUND)
    set(AI_SUBTITLER_SDL2_TARGETS SDL2::SDL2)
else()
    find_package(SDL2 REQUIRED)
endif()

add_executable(ai-subtitler-streamerbot
    src/main.cpp
    src/streamerbot_ws_client_winhttp.cpp
    src/streamerbot_ws_client.h
    submodules/whisper.cpp/examples/common.cpp
    submodules/whisper.cpp/examples/common-whisper.cpp
    submodules/whisper.cpp/examples/common-sdl.cpp
)

target_include_directories(ai-subtitler-streamerbot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/whisper.cpp/examples
)

if (SDL2_FOUND)
    target_link_libraries(ai-subtitler-streamerbot PRIVATE ${AI_SUBTITLER_SDL2_TARGETS})
else()
    target_include_directories(ai-subtitler-streamerbot PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(ai-subtitler-streamerbot PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(ai-subtitler-streamerbot PRIVATE
    whisper
)

if (WIN32)
    target_compile_definitions(ai-subtitler-streamerbot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ai-subtitler-streamerbot PRIVATE winhttp crypt32 bcrypt)
endif()
