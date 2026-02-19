# Minimal FindSDL2.cmake
#
# Supports locating an extracted SDL2 development package on Windows.
#
# Variables you can set:
#   SDL2_ROOT   - root of SDL2 dev package (contains include/SDL.h)
#   SDL2_DIR    - alternative root
#
# Outputs:
#   SDL2_FOUND
#   SDL2_INCLUDE_DIRS
#   SDL2_LIBRARIES
#   SDL2::SDL2 (imported target, when library is found)

include(FindPackageHandleStandardArgs)

set(_SDL2_HINTS)
if (SDL2_ROOT)
    list(APPEND _SDL2_HINTS "${SDL2_ROOT}")
endif()
if (SDL2_DIR)
    list(APPEND _SDL2_HINTS "${SDL2_DIR}")
endif()
if (DEFINED ENV{SDL2_ROOT})
    list(APPEND _SDL2_HINTS "$ENV{SDL2_ROOT}")
endif()
if (DEFINED ENV{SDL2_DIR})
    list(APPEND _SDL2_HINTS "$ENV{SDL2_DIR}")
endif()

find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    HINTS ${_SDL2_HINTS}
    PATH_SUFFIXES include include/SDL2 SDL2/include
)

set(_SDL2_LIB_HINTS ${_SDL2_HINTS})
if (SDL2_INCLUDE_DIR)
    get_filename_component(_SDL2_ROOT_FROM_INC "${SDL2_INCLUDE_DIR}" DIRECTORY)
    list(APPEND _SDL2_LIB_HINTS "${_SDL2_ROOT_FROM_INC}")
endif()

find_library(SDL2_LIBRARY
    NAMES SDL2 SDL2-static
    HINTS ${_SDL2_LIB_HINTS}
    PATH_SUFFIXES lib lib/x64 lib/x86 x64/x64/lib x86/x86/lib
)

find_package_handle_standard_args(SDL2
    REQUIRED_VARS SDL2_INCLUDE_DIR SDL2_LIBRARY
)

if (SDL2_FOUND)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
    set(SDL2_LIBRARIES ${SDL2_LIBRARY})

    if (NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 UNKNOWN IMPORTED)
        set_target_properties(SDL2::SDL2 PROPERTIES
            IMPORTED_LOCATION "${SDL2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}"
        )
    endif()
endif()
