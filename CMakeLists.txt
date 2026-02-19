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
option(AI_SUBTITLER_FETCH_SDL2 "Fetch and build SDL2 if not found" ON)

find_package(SDL2 CONFIG QUIET)
if (SDL2_FOUND)
    set(AI_SUBTITLER_SDL2_TARGETS SDL2::SDL2)
else()
    find_package(SDL2 QUIET) # uses cmake/FindSDL2.cmake if present
    if (SDL2_FOUND)
        set(AI_SUBTITLER_SDL2_TARGETS SDL2::SDL2)
    endif()
endif()

if (NOT SDL2_FOUND AND AI_SUBTITLER_FETCH_SDL2)
    include(FetchContent)

    # Build SDL2 from source as a fallback (useful on fresh Windows installs).
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.10
    )
    FetchContent_MakeAvailable(SDL2)

    if (TARGET SDL2::SDL2)
        set(SDL2_FOUND TRUE)
        set(AI_SUBTITLER_SDL2_TARGETS SDL2::SDL2)
    elseif(TARGET SDL2)
        set(SDL2_FOUND TRUE)
        set(AI_SUBTITLER_SDL2_TARGETS SDL2)
    endif()
endif()

if (NOT SDL2_FOUND)
    message(FATAL_ERROR "SDL2 not found. Set SDL2_ROOT/SDL2_DIR to an SDL2 dev package, install via vcpkg, or enable AI_SUBTITLER_FETCH_SDL2=ON")
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

target_link_libraries(ai-subtitler-streamerbot PRIVATE ${AI_SUBTITLER_SDL2_TARGETS})

target_link_libraries(ai-subtitler-streamerbot PRIVATE
    whisper
)

if (WIN32)
    target_compile_definitions(ai-subtitler-streamerbot PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_definitions(ai-subtitler-streamerbot PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(ai-subtitler-streamerbot PRIVATE winhttp crypt32 bcrypt)

    # Make the exe runnable directly by copying required runtime DLLs.
    add_custom_command(TARGET ai-subtitler-streamerbot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:whisper> $<TARGET_FILE_DIR:ai-subtitler-streamerbot>
        VERBATIM)

    if (TARGET ggml)
        add_custom_command(TARGET ai-subtitler-streamerbot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ggml> $<TARGET_FILE_DIR:ai-subtitler-streamerbot>
            VERBATIM)
    endif()
    if (TARGET ggml-base)
        add_custom_command(TARGET ai-subtitler-streamerbot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ggml-base> $<TARGET_FILE_DIR:ai-subtitler-streamerbot>
            VERBATIM)
    endif()
    if (TARGET ggml-cpu)
        add_custom_command(TARGET ai-subtitler-streamerbot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ggml-cpu> $<TARGET_FILE_DIR:ai-subtitler-streamerbot>
            VERBATIM)
    endif()
    if (TARGET SDL2::SDL2)
        add_custom_command(TARGET ai-subtitler-streamerbot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:ai-subtitler-streamerbot>
            VERBATIM)
    endif()
endif()
