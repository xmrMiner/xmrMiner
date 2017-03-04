cmake_minimum_required(VERSION 3.3.0)


################################################################################
# Project
################################################################################

project(xmrMiner)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE PATH "install prefix" FORCE)
endif(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)

list(APPEND CMAKE_PREFIX_PATH "$ENV{CUDA_ROOT}")
list(APPEND CMAKE_PREFIX_PATH "$ENV{CMAKE_PREFIX_PATH}")

set(BUILD_TYPE "Release;Debug")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${BUILD_TYPE}")

option(xmrMiner_LARGEGRID "Support large CUDA block count > 128" ON)
if(xmrMiner_LARGEGRID)
    add_definitions("-DXMRMINER_LARGEGRID=${xmrMiner_LARGEGRID}")
endif()

set(DEVICE_COMPILER "nvcc")
set(CUDA_COMPILER "${DEVICE_COMPILER}" CACHE STRING "Select the device compiler")

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    list(APPEND DEVICE_COMPILER "clang")
endif()

set_property(CACHE CUDA_COMPILER PROPERTY STRINGS "${DEVICE_COMPILER}")

################################################################################
# Find CUDA
################################################################################

find_package(CUDA 6.0 REQUIRED)
set(LIBS ${LIBS} ${CUDA_LIBRARIES})

set(xmrMiner_THREADS 0 CACHE STRING "Set maximum number of threads (for compile time optimization)")
if(NOT xmrMiner_THREADS EQUAL 0)
    message(STATUS "xmrMiner: set max threads per block to ${xmrMiner_THREADS}")
    add_definitions("-DXMRMINER_THREADS=${xmrMiner_THREADS}")
endif()

set(CUDA_ARCH "20;30;35;37;50;52;60;61;62" CACHE STRING "Set GPU architecture (semicolon separated list, e.g. '-DCUDA_ARCH=20;35;60')")

foreach(CUDA_ARCH_ELEM ${CUDA_ARCH})
    string(REGEX MATCH "^[0-9]+$" IS_NUMBER ${CUDA_ARCH})
    if(NOT IS_NUMBER)
        message(FATAL_ERROR "Defined compute architecture '${CUDA_ARCH_ELEM}' in "
                            "'${CUDA_ARCH}' is not an integral number, use e.g. '30' (for compute architecture 3.0).")
    endif()
    unset(IS_NUMBER)

    if(${CUDA_ARCH_ELEM} LESS 20)
        message(FATAL_ERROR "Unsupported CUDA architecture '${CUDA_ARCH_ELEM}' specified. "
                            "Use '30' (for compute architecture 3.0) or higher.")
    endif()
endforeach()

option(CUDA_SHOW_REGISTER "Show registers used for each kernel and compute architecture" OFF)
option(CUDA_KEEP_FILES "Keep all intermediate files that are generated during internal compilation steps" OFF)

if("${CUDA_COMPILER}" STREQUAL "clang")
set(LIBS ${LIBS} cudart_static)

#set(LIBS ${LIBS} cudart_static)
    set(CLANG_BUILD_FLAGS "-O3 -x cuda --cuda-path=${CUDA_TOOLKIT_ROOT_DIR}")
    # activation usage of FMA
    set(CLANG_BUILD_FLAGS "${CLANG_BUILD_FLAGS} -ffp-contract=fast")

    if(CUDA_SHOW_REGISTER)
        set(CLANG_BUILD_FLAGS "${CLANG_BUILD_FLAGS} -Xcuda-ptxas -v")
    endif(CUDA_SHOW_REGISTER)

    if(CUDA_KEEP_FILES)
        set(CLANG_BUILD_FLAGS "${CLANG_BUILD_FLAGS} -save-temps=${PROJECT_BINARY_DIR}")
    endif(CUDA_KEEP_FILES)

    foreach(CUDA_ARCH_ELEM ${CUDA_ARCH})
        # set flags to create device code for the given architectures
        set(CLANG_BUILD_FLAGS "${CLANG_BUILD_FLAGS} --cuda-gpu-arch=sm_${CUDA_ARCH_ELEM}")
    endforeach()

elseif("${CUDA_COMPILER}" STREQUAL "nvcc")

    foreach(CUDA_ARCH_ELEM ${CUDA_ARCH})
        # set flags to create device code for the given architecture
        set(CUDA_NVCC_FLAGS ${CUDA_NVCC_FLAGS}
            "--generate-code arch=compute_${CUDA_ARCH_ELEM},code=sm_${CUDA_ARCH_ELEM} --generate-code arch=compute_${CUDA_ARCH_ELEM},code=compute_${CUDA_ARCH_ELEM}")
    endforeach()

    option(CUDA_SHOW_CODELINES "Show kernel lines in cuda-gdb and cuda-memcheck" OFF)

    if(CUDA_SHOW_CODELINES)
        set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS}" --source-in-ptx -lineinfo)
        set(CUDA_KEEP_FILES ON CACHE BOOL "activate keep files" FORCE)
    endif(CUDA_SHOW_CODELINES)

    if(CUDA_SHOW_REGISTER)
        set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS}" -Xptxas=-v)
    endif(CUDA_SHOW_REGISTER)

    if(CUDA_KEEP_FILES)
        set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS}" --keep --keep-dir "${PROJECT_BINARY_DIR}")
    endif(CUDA_KEEP_FILES)

    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        string(STRIP ${CMAKE_CXX_COMPILER_VERSION} CMAKE_CXX_COMPILER_VERSION)
        string(REGEX REPLACE "^([0-9]+).*$" "\\1"
               CMAKE_CXX_COMPILER_MAJOR ${CMAKE_CXX_COMPILER_VERSION})

        if( ${CMAKE_CXX_COMPILER_MAJOR} GREATER 5)
             # allow compile with gnu 6.x
            set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS}" --std=c++11)
        endif()
    endif()

else()

    message(FATAL_ERROR "selected CUDA compiler '${CUDA_COMPILER}' is not supported")

endif()

################################################################################
# Find PThreads
################################################################################

find_package(Threads REQUIRED)
set(LIBS ${LIBS} ${CMAKE_THREAD_LIBS_INIT})

###############################################################################
# Find JANSSON
###############################################################################
find_package(Jansson QUIET CONFIG)
# if we can't find the jansson cmake config we need to manually search for the libs and header
if(NOT JANSSON_FOUND)
  find_path(JANSSON_INCLUDE_DIR
    NAMES
        jansson.h
    PATHS
        /usr/include
        /usr/local/include
        /opt/local/include
        /sw/include
    )

    find_library(JANSSON_LIBRARY
        NAMES
            jansson
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            /sw/lib/
            usr/lib/x86_64-linux-gnu/
    )

    set(JANSSON_INCLUDE_DIRS
      ${JANSSON_INCLUDE_DIR}
    )

    if(JANSSON_LIBRARY)
      set(JANSSON_LIBRARIES
        ${JANSSON_LIBRARIES}
        ${JANSSON_LIBRARY}
    )
    endif()

    if(NOT JANSSON_LIBRARIES OR NOT JANSSON_INCLUDE_DIRS)
        message(FATAL_ERROR "Jansson - not found")
    endif()
endif()

set(LIBS ${LIBS} ${JANSSON_LIBRARIES})
set(XMRMINER_INLCUDES ${XMRMINER_INLCUDES} ${JANSSON_INCLUDE_DIRS})



###############################################################################
# Find CURL
###############################################################################

find_package(CURL REQUIRED)
set(XMRMINER_INLCUDES ${XMRMINER_INLCUDES} ${CURL_INCLUDE_DIR})
set(LIBS ${LIBS} ${CURL_LIBRARIES})


include_directories(.)
include_directories(${XMRMINER_INLCUDES})
include_directories(${PROJECT_BINARY_DIR})


include(CheckIncludeFiles)
include(CheckFunctionExists)
include(CheckPrototypeDefinition)

# Define to 1 if you have the <syslog.h> header file.
check_function_exists(syslog.h HAVE_SYSLOG_H)
# Define to 1 if you have the `getopt_long' function.
check_function_exists(getopt_long HAVE_GETOPT_LONG)
# Define to 1 if you have the <alloca.h> header file.
check_include_files(alloca.h HAVE_ALLOCA_H)
# Define to 1 if you have the `alloca' function.
check_function_exists(alloca HAVE_ALLOCA)
# Define to 1 if you have the <stdlib.h> and <stddef.h> header file.
check_include_files("stdlib.h;stddef.h" STDC_HEADERS)
# Define to 1 if you have the <stdlib.h> header file.
check_include_files(stdlib.h HAVE_STDLIB_H)
# Define to 1 if you have the <sys/endian.h> header file.
check_include_files(sys/endian.h HAVE_SYS_ENDIAN_H)
# Define to 1 if you have the declaration of `be32dec'
check_prototype_definition(be32dec "uint32_t be32dec(const void *)" "0" "sys/endian.h" HAVE_DECL_BE32DEC)
# Define to 1 if you have the declaration of `be32enc'
check_prototype_definition(be32enc "void be32enc(void *, uint32_t)" ""  "sys/endian.h" HAVE_DECL_BE32ENC)
# Define to 1 if you have the declaration of `le32dec'
check_prototype_definition(le32dec "uint32_t le32dec(const void *)" "0" "sys/endian.h" HAVE_DECL_LE32DEC)
# Define to 1 if you have the declaration of `le32enc'
check_prototype_definition(le32enc "void le32enc(void *, uint32_t)" "" "sys/endian.h" HAVE_DECL_LE32ENC)

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/xmrMiner-config.h.cmake ${PROJECT_BINARY_DIR}/xmrMiner-config.h)

################################################################################
# Compile & Link PIConGPU
################################################################################

file(GLOB CUDASRCFILES "cryptonight/*.cu")
file(GLOB SRCFILES "*.c")
file(GLOB SRCFILES_CRYPTO "crypto/*.c")

if("${CUDA_COMPILER}" STREQUAL "clang")
    add_library(xmrMinerCuda
        STATIC
        ${CUDASRCFILES}
    )
    set_target_properties(xmrMinerCuda PROPERTIES COMPILE_FLAGS ${CLANG_BUILD_FLAGS})
    set_target_properties(xmrMinerCuda PROPERTIES LINKER_LANGUAGE CXX)
    set_source_files_properties(${CUDASRCFILES} PROPERTIES LANGUAGE CXX)
else()
    cuda_add_library(xmrMinerCuda
        STATIC
        ${CUDASRCFILES}
    )
endif()

add_library(xmrMinerCrypto
    STATIC
    ${SRCFILES_CRYPTO}
)
set_property(TARGET xmrMinerCrypto PROPERTY C_STANDARD 99)

add_executable(xmrMiner
    ${SRCFILES}
)
set_property(TARGET xmrMiner PROPERTY C_STANDARD 99)

target_link_libraries(xmrMiner ${LIBS} xmrMinerCuda xmrMinerCrypto)

install(TARGETS xmrMiner
         RUNTIME DESTINATION .)
