cmake_minimum_required(VERSION 3.16)
project(sdl2_gl33_template C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist")

# Find SDL2: prefer the CMake package, fall back to pkg-config
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    set(SDL2_LINK_TARGET SDL2::SDL2)
    set(SDL2MAIN_LINK_TARGET SDL2::SDL2main)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2_PC REQUIRED IMPORTED_TARGET sdl2)
    set(SDL2_LINK_TARGET PkgConfig::SDL2_PC)
    # pkg-config doesn't list SDL2main, so find its library directly
    find_library(SDL2MAIN_LIB NAMES SDL2main REQUIRED)
    set(SDL2MAIN_LINK_TARGET ${SDL2MAIN_LIB})
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/glad.c
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2MAIN_LINK_TARGET} ${SDL2_LINK_TARGET})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    set(SDL2_RUNTIME_HINTS "")
    if(DEFINED SDL2_PC_LIBDIR)
        list(APPEND SDL2_RUNTIME_HINTS "${SDL2_PC_LIBDIR}/../bin")
    endif()
    if(DEFINED ENV{MSYSTEM_PREFIX})
        list(APPEND SDL2_RUNTIME_HINTS "$ENV{MSYSTEM_PREFIX}/bin")
    endif()
    list(APPEND SDL2_RUNTIME_HINTS "C:/msys64/mingw64/bin")

    find_file(SDL2_DLL NAMES SDL2.dll HINTS ${SDL2_RUNTIME_HINTS} NO_DEFAULT_PATH)
    if(NOT SDL2_DLL)
        message(FATAL_ERROR "SDL2.dll not found. Checked: ${SDL2_RUNTIME_HINTS}")
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL2_DLL}"
                "${DIST_DIR}"
        COMMENT "Copying executable and SDL2.dll to ${DIST_DIR}")
else()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${PROJECT_NAME}>"
                "${DIST_DIR}"
        COMMENT "Copying executable to ${DIST_DIR}")
endif()