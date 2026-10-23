cmake_minimum_required(VERSION 3.20)

# On a non-Windows host, auto-select a mingw-w64 cross toolchain when one is
# available, so the plain `cmake -S . -B build -G Ninja` from the README
# cross-builds a Windows .exe (e.g. Debian with gcc-mingw-w64-x86-64-posix
# installed). An explicit toolchain file or `-DCMAKE_SYSTEM_NAME=Windows` /
# `-DCMAKE_C_COMPILER=...` / `-DCMAKE_RC_COMPILER=...` always takes precedence.
if(NOT WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    find_program(WISTRIBUTE_MINGW_CC
        NAMES x86_64-w64-mingw32-gcc x86_64-w64-mingw32-gcc-posix)
    if(WISTRIBUTE_MINGW_CC)
        set(CMAKE_SYSTEM_NAME "Windows" CACHE STRING "" FORCE)
        set(CMAKE_SYSTEM_PROCESSOR "x86_64" CACHE STRING "" FORCE)
        set(CMAKE_C_COMPILER "${WISTRIBUTE_MINGW_CC}" CACHE FILEPATH "" FORCE)
        find_program(WISTRIBUTE_MINGW_RC NAMES x86_64-w64-mingw32-windres)
        if(WISTRIBUTE_MINGW_RC)
            set(CMAKE_RC_COMPILER "${WISTRIBUTE_MINGW_RC}" CACHE FILEPATH "" FORCE)
        endif()
    endif()
endif()

project(wistribute LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "wistribute targets Windows only. "
        "On Linux, cross-build with: "
        "cmake -S . -B build-w64 -G Ninja "
        "-DCMAKE_SYSTEM_NAME=Windows "
        "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc "
        "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres "
        "(install gcc-mingw-w64-x86-64-posix).")
endif()

# --- options -----------------------------------------------------------------
option(WISTRIBUTE_BUILD_DOWNLOAD "Build the download-mode exe"  ON)
option(WISTRIBUTE_BUILD_EMBEDDED "Build the embedded-mode exe"  ON)

set(ZBS_URL    "https://github.com/pkulchenko/ZeroBraneStudio/archive/refs/heads/master.zip"
    CACHE STRING "Source zip URL embedded into the offline exe")
set(MINIZ_TAG  "3.1.1" CACHE STRING "miniz release tag to vendor")

# --- vendored deps (downloaded at configure time) ----------------------------
set(MINIZ_DIR "${CMAKE_BINARY_DIR}/_deps/miniz")
file(MAKE_DIRECTORY "${MINIZ_DIR}")

if(NOT (EXISTS "${MINIZ_DIR}/miniz.c" AND EXISTS "${MINIZ_DIR}/miniz.h"))
    set(_miniz_zip "${MINIZ_DIR}/miniz-${MINIZ_TAG}.zip")
    message(STATUS "Fetching miniz ${MINIZ_TAG} amalgamation zip")
    file(DOWNLOAD
        "https://github.com/richgel999/miniz/releases/download/${MINIZ_TAG}/miniz-${MINIZ_TAG}.zip"
        "${_miniz_zip}"
        SHOW_PROGRESS STATUS dl_status TLS_VERIFY ON)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        file(REMOVE "${_miniz_zip}")
        message(FATAL_ERROR "Failed to download miniz zip: ${dl_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_miniz_zip}" DESTINATION "${MINIZ_DIR}"
         PATTERNS miniz.c miniz.h)
endif()

# --- payload zip for the embedded build --------------------------------------
set(PAYLOAD_ZIP "${CMAKE_BINARY_DIR}/_deps/payload/zbs.zip")
if(WISTRIBUTE_BUILD_EMBEDDED AND NOT EXISTS "${PAYLOAD_ZIP}")
    message(STATUS "Fetching payload zip from ${ZBS_URL}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/payload")
    file(DOWNLOAD "${ZBS_URL}" "${PAYLOAD_ZIP}"
         SHOW_PROGRESS STATUS dl_status TLS_VERIFY ON)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        file(REMOVE "${PAYLOAD_ZIP}")
        message(FATAL_ERROR "Failed to download payload zip: ${dl_status}")
    endif()
endif()

# --- shared sources ----------------------------------------------------------
set(COMMON_SRC
    src/main.c
    src/unzip.c
    src/gui.c
    "${MINIZ_DIR}/miniz.c"
    res/wistribute.rc)

set(COMMON_INCLUDES src "${MINIZ_DIR}")
set(COMMON_LIBS comctl32 shell32 ole32 uuid user32 gdi32)

# Common compile flags
add_library(wistribute_flags INTERFACE)
target_compile_options(wistribute_flags INTERFACE
    -municode -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -O2)
target_link_options(wistribute_flags INTERFACE
    -municode -mwindows -s)
target_compile_definitions(wistribute_flags INTERFACE
    UNICODE _UNICODE)

# miniz amalgamation has many unused-function warnings — silence per-file.
set_source_files_properties("${MINIZ_DIR}/miniz.c"
    PROPERTIES COMPILE_FLAGS "-w")

# .rc compiler needs to find the manifest by relative path
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_SOURCE_DIR}/res")

# --- download-mode exe -------------------------------------------------------
if(WISTRIBUTE_BUILD_DOWNLOAD)
    add_executable(wistribute WIN32 ${COMMON_SRC} src/http.c)
    target_include_directories(wistribute PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(wistribute PRIVATE wistribute_flags ${COMMON_LIBS} winhttp)
    target_compile_definitions(wistribute PRIVATE MODE_DOWNLOAD)
endif()

# --- embedded-mode exe -------------------------------------------------------
if(WISTRIBUTE_BUILD_EMBEDDED)
    # Copy payload next to embed.c so its `#embed "../payload/zbs.zip"` works
    # both from source-tree builds and out-of-source builds.
    set(EMBED_PAYLOAD_DIR "${CMAKE_BINARY_DIR}/payload")
    file(MAKE_DIRECTORY "${EMBED_PAYLOAD_DIR}")
    configure_file("${PAYLOAD_ZIP}" "${EMBED_PAYLOAD_DIR}/zbs.zip" COPYONLY)

    set(EMBED_SRC_DIR "${CMAKE_BINARY_DIR}/embed_src")
    file(MAKE_DIRECTORY "${EMBED_SRC_DIR}")
    configure_file("${CMAKE_SOURCE_DIR}/src/embed.c" "${EMBED_SRC_DIR}/embed.c" COPYONLY)

    # Fallback byte-array include for compilers without C23 #embed (e.g.
    # mingw-w64 GCC < 15). Generated with `xxd -i` under a deterministic
    # symbol name; embed.c #includes it only when #embed is unavailable.
    find_program(WISTRIBUTE_XXD xxd)
    if(NOT WISTRIBUTE_XXD)
        message(FATAL_ERROR "xxd is required to embed the payload (install vim-common)")
    endif()
    execute_process(
        COMMAND "${WISTRIBUTE_XXD}" -i -n ZBS_ZIP zbs.zip
        WORKING_DIRECTORY "${EMBED_PAYLOAD_DIR}"
        RESULT_VARIABLE _xxd_rc
        OUTPUT_VARIABLE _xxd_out
        ERROR_QUIET)
    if(NOT _xxd_rc EQUAL 0)
        message(FATAL_ERROR "xxd failed to generate the embedded payload array")
    endif()
    string(REGEX REPLACE "^unsigned char " "static const unsigned char " _xxd_out "${_xxd_out}")
    file(WRITE "${EMBED_SRC_DIR}/zbs_zip.inc" "${_xxd_out}")

    add_executable(wistribute-offline WIN32 ${COMMON_SRC} "${EMBED_SRC_DIR}/embed.c")
    target_include_directories(wistribute-offline PRIVATE ${COMMON_INCLUDES} "${EMBED_SRC_DIR}")
    target_link_libraries(wistribute-offline PRIVATE wistribute_flags ${COMMON_LIBS})
    target_compile_definitions(wistribute-offline PRIVATE MODE_EMBEDDED)
endif()
