cmake_minimum_required(VERSION 2.8)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake;${CMAKE_MODULE_PATH}")
include(Utilities)
project(KinesisVideoProducerCpp)

set(CMAKE_CXX_STANDARD 11)

# User Flags
option(BUILD_STATIC "Build with static linkage." OFF)
option(ADD_MUCLIBC "Add -muclibc c flag" OFF)
option(BUILD_DEPENDENCIES "Whether or not to build depending libraries from source" ON)
option(BUILD_OPENSSL_PLATFORM "If buildng OpenSSL what is the target platform" OFF)

# Developer Flags
option(BUILD_TEST "Build the testing tree." OFF)
option(CODE_COVERAGE "Enable coverage reporting" OFF)
option(COMPILER_WARNINGS "Enable all compiler warnings." OFF)
option(ADDRESS_SANITIZER "Build with AddressSanitizer." OFF)
option(MEMORY_SANITIZER "Build with MemorySanitizer." OFF)
option(THREAD_SANITIZER "Build with ThreadSanitizer." OFF)
option(UNDEFINED_BEHAVIOR_SANITIZER "Build with UndefinedBehaviorSanitizer." OFF)

set(CMAKE_MACOSX_RPATH TRUE)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)

set(KINESIS_VIDEO_PIC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/open-source/amazon-kinesis-video-streams-pic/")
set(KINESIS_VIDEO_PRODUCER_C_SRC "${CMAKE_CURRENT_SOURCE_DIR}/open-source/amazon-kinesis-video-streams-producer-c/")
set(KINESIS_VIDEO_PRODUCER_CPP_SRC "${CMAKE_SOURCE_DIR}")
set(KINESIS_VIDEO_OPEN_SOURCE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/open-source)


message(STATUS "Kinesis Video PIC path is ${KINESIS_VIDEO_PIC_SRC}")
message(STATUS "Kinesis Video C Producer path is ${KINESIS_VIDEO_PRODUCER_C_SRC}")
message(STATUS "Kinesis Video WebRTC Client path is ${KINESIS_VIDEO_PRODUCER_CPP_SRC}")
message(STATUS "dependencies install path is ${KINESIS_VIDEO_OPEN_SOURCE_SRC}")

if(NOT EXISTS ${KINESIS_VIDEO_PIC_SRC})
  message(FATAL_ERROR "KINESIS_VIDEO_PIC_SRC ${KINESIS_VIDEO_PIC_SRC} does not exist")
elseif(NOT EXISTS ${KINESIS_VIDEO_PRODUCER_CPP_SRC})
  message(FATAL_ERROR "KINESIS_VIDEO_PRODUCER_CPP_SRC ${KINESIS_VIDEO_PRODUCER_CPP_SRC} does not exist")
elseif(NOT EXISTS ${KINESIS_VIDEO_PRODUCER_C_SRC})
  message(FATAL_ERROR "KINESIS_VIDEO_PRODUCER_C_SRC ${KINESIS_VIDEO_PRODUCER_C_SRC} does not exist")
endif()

# pass ca cert location to sdk
add_definitions(-DKVS_CA_CERT_PATH="${CMAKE_SOURCE_DIR}/certs/cert.pem")
add_definitions(-DCMAKE_DETECTED_CACERT_PATH)

if(BUILD_DEPENDENCIES)
  if(NOT EXISTS ${KINESIS_VIDEO_OPEN_SOURCE_SRC})
    file(MAKE_DIRECTORY ${KINESIS_VIDEO_OPEN_SOURCE_SRC}/local)
  endif()

  set(OPEN_SRC_INSTALL_PREFIX ${KINESIS_VIDEO_OPEN_SOURCE_SRC}/local)
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${OPEN_SRC_INSTALL_PREFIX}/lib/pkgconfig")
  set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} ${OPEN_SRC_INSTALL_PREFIX})

  message(STATUS "Begin building dependencies.")

  build_dependency(openssl -DBUILD_STATIC=${BUILD_STATIC} -DBUILD_OPENSSL_PLATFORM=${BUILD_OPENSSL_PLATFORM})
  build_dependency(jsmn)

  if(BUILD_TEST)
    build_dependency(gtest)
  endif()

  message(STATUS "Finished building dependencies.")
endif()

############# find dependent libraries ############
find_package(Threads)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)

set(OPEN_SRC_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR} ${CURL_INCLUDE_DIRS})

find_library(Jsmn
  NAMES jsmn REQUIRED
  PATHS ${OPEN_SRC_INSTALL_PREFIX}/lib)

############# Enable Sanitizers ############
if("${CMAKE_C_COMPILER_ID}" MATCHES "GNU|Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

  if(ADD_MUCLIBC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -muclibc")
  endif()

  if(CODE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
  endif()

  if(ADDRESS_SANITIZER)
    enableSanitizer("address")
  endif()
  if(MEMORY_SANITIZER)
    enableSanitizer("memory")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize-memory-track-origins")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize-memory-track-origins")
  endif()
  if(THREAD_SANITIZER)
    enableSanitizer("thread")
  endif()
  if(UNDEFINED_BEHAVIOR_SANITIZER)
    enableSanitizer("undefined")
  endif()
endif()

############# Build Targets ############
file(GLOB PRODUCER_CPP_SOURCE_FILES "src/*.cpp")
file(GLOB PIC_HEADERS "${KINESIS_VIDEO_PIC_SRC}/src/*/include")

include_directories(${PIC_HEADERS})
include_directories(${OPEN_SRC_INCLUDE_DIRS})
include_directories(${KINESIS_VIDEO_OPEN_SOURCE_SRC}/include)
include_directories(${KINESIS_VIDEO_PRODUCER_CPP_SRC}/src)
include_directories(${KINESIS_VIDEO_PRODUCER_C_SRC}/src/include)

set(BUILD_COMMON_LWS
    FALSE
    CACHE BOOL "Build ProducerC without LWS Support" FORCE)
set(BUILD_COMMON_CURL
    TRUE
    CACHE BOOL "Build ProducerC with CURL Support" FORCE)
add_subdirectory(${KINESIS_VIDEO_PRODUCER_C_SRC} ./kvscproducer EXCLUDE_FROM_ALL)

add_library(producer STATIC ${PRODUCER_CPP_SOURCE_FILES})
