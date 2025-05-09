cmake_minimum_required(VERSION 3.13)

get_filename_component(root ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
include(${root}/deps/readies/cmake/main)


get_filename_component(binroot ${CMAKE_CURRENT_BINARY_DIR}/.. ABSOLUTE)

#----------------------------------------------------------------------------------------------

option(BUILD_STATIC "Build a static library" OFF)
option(USE_REDIS_ALLOCATOR "Use redis allocator" ON)
option(BUILD_SEARCH_UNIT_TESTS "Build unit tests" OFF)

#----------------------------------------------------------------------------------------------

project(redisearch)

if (NOT MODULE_NAME)
    set(MODULE_NAME "search")  # unless it was already set to "searchlite"
endif()

#----------------------------------------------------------------------------------------------

setup_cc_options()

# ugly hack for cpu_features::list_cpu_features coming from VecSim
set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} ${CMAKE_LD_FLAGS}")

message(STATUS "SAN: ${SAN}")

# check if env sanitizers are set, and set ld_flags accordingly
if (SAN)
    if (SAN STREQUAL "address")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address -fsanitize-recover=all")
        set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
        # print the flags
        message(STATUS "CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
        message(STATUS "CMAKE_LINKER_FLAGS: ${CMAKE_LINKER_FLAGS}")
        message(STATUS "CMAKE_SHARED_LINKER_FLAGS: ${CMAKE_SHARED_LINKER_FLAGS}")
    endif()
endif()

# The default signedness of char can vary by architecture and compiler.
# Common defaults:
# - x86/x86_64: Typically signed by default.
# - ARM: Typically unsigned by default.
# This can lead to different behavior when comparing or converting char to int,
# and causes different behavior between different platforms when sorting strings.
# To avoid this, we set the default signedness of char to unsigned.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -funsigned-char")

add_compile_definitions(
    "REDISEARCH_MODULE_NAME=\"${MODULE_NAME}\""
    "GIT_VERSPEC=\"${GIT_VERSPEC}\""
    "GIT_SHA=\"${GIT_SHA}\""
    REDISMODULE_SDK_RLEC
    _GNU_SOURCE)

if (BUILD_LITE)
    message(WARNING "Building artifact under the name searchlight for backward compatibility, but coordinator will be activated in clustered environment")
endif()

if (MAX_WORKER_THREADS)
    set_source_files_properties(src/config.c PROPERTIES COMPILE_DEFINITIONS MAX_WORKER_THREADS=${MAX_WORKER_THREADS})
endif()

if (USE_REDIS_ALLOCATOR)
    add_compile_definitions(REDIS_MODULE_TARGET)
endif()

if (APPLE)
    include_directories(/usr/local/opt/openssl/include)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L/usr/local/opt/openssl/lib")

    include_directories(${LIBSSL_DIR}/include)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L${LIBSSL_DIR}/lib")
    set(SSL_LIBS crypto ssl)
else()
    set(SSL_LIBS crypto crypt ssl)
endif()

# On debug artifacts, enable assertions
if (DEBUG)
    add_compile_definitions(ENABLE_ASSERT=1)
endif()

#----------------------------------------------------------------------------------------------

include(${root}/build/libuv/libuv.cmake)
include(${root}/build/hiredis/hiredis.cmake)
include(${root}/build/boost/boost.cmake)
if (NOT IS_DIRECTORY ${BOOST_DIR})
    message(FATAL_ERROR "BOOST_DIR is not defined or does not point to a valid directory ${BOOST_DIR}")
endif()

set(BOOST_ROOT ${BOOST_DIR})
set(Boost_NO_WARN_NEW_VERSIONS ON)

include_directories(
    ${root}/src
    ${root}/src/coord
    ${root}/src/redisearch_rs
    ${root}/deps/libuv/include
    ${root}/deps
    ${root}/deps/triemap
    ${root}/deps/RedisModulesSDK
    ${root}/deps/VectorSimilarity/src
    ${BOOST_DIR}
    ${root})

add_subdirectory(deps/rmutil)
add_subdirectory(deps/friso)
add_subdirectory(deps/snowball)
add_subdirectory(deps/phonetics)
add_subdirectory(deps/fast_float)
add_subdirectory(deps/triemap)

option(VECSIM_STATIC "Build as static library" ON)
option(VECSIM_BUILD_TESTS "Build vecsim tests" OFF)

add_subdirectory(deps/VectorSimilarity)
add_subdirectory(src/geometry)
add_subdirectory(src/util/hash)
add_subdirectory(src/coord)

#----------------------------------------------------------------------------------------------

file(GLOB SOURCES
    "src/*.c"
    "src/aggregate/*.c"
    "src/aggregate/expr/*.c"
    "src/aggregate/functions/*.c"
    "src/aggregate/reducers/*.c"
    "src/ext/*.c"
    "src/hll/*.c"
    "src/query_parser/v1/*.c"
    "src/query_parser/v2/*.c"
    "src/util/*.c"
    "src/trie/*.c"
    "src/info/*.c"
    "src/info/info_redis/*.c"
    "src/info/info_redis/threads/*.c"
    "src/info/info_redis/types/*.c"
    "src/module-init/*.c"
    "src/obfuscation/*.c"

    "deps/cndict/cndict_data.c"
    "deps/libnu/*.c"
    "deps/miniz/*.c"
    "deps/base64/*.c"
    "deps/fast_float/*.c"
    "deps/thpool/*.c"
    "deps/geohash/*.c")

add_library(rscore OBJECT ${SOURCES})

set(FINAL_OBJECTS
    $<TARGET_OBJECTS:trie>
    $<TARGET_OBJECTS:rscore>
    $<TARGET_OBJECTS:rmutil>
    $<TARGET_OBJECTS:friso>
    $<TARGET_OBJECTS:snowball>
    $<TARGET_OBJECTS:metaphone>
    $<TARGET_OBJECTS:fast_float_strtod>
    $<TARGET_OBJECTS:redisearch-coord>
)

#----------------------------------------------------------------------------------------------

if (BUILD_STATIC)
    add_library(redisearch-static STATIC ${FINAL_OBJECTS})
    add_library(redisearch SHARED ${FINAL_OBJECTS})

    set_target_properties(redisearch PROPERTIES LINKER_LANGUAGE CXX)
    setup_shared_object_target(redisearch "")
    target_link_libraries(redisearch redisearch-geometry redisearch-hash VectorSimilarity trie ${REDISEARCH_LIBS} ${CMAKE_LD_LIBS})
    target_link_libraries(redisearch ${binroot}/redisearch_rs/libredisearch_rs.a)

    set(TEST_MODULE "redisearch-static")
    set(TEST_MODULE_SO $<TARGET_FILE:redisearch>)

else() # OSS RediSearch
    add_library(redisearch SHARED ${FINAL_OBJECTS})

    set_target_properties(redisearch PROPERTIES LINKER_LANGUAGE CXX)
    setup_shared_object_target(redisearch "")
    target_link_libraries(redisearch
            redisearch-geometry
            redisearch-hash
            VectorSimilarity
            redisearch-coord
            trie
            ${LIBUV_LIBS}
            ${HIREDIS_LIBS}
            ${SSL_LIBS}
            ${REDISEARCH_LIBS}
            ${CMAKE_LD_LIBS})
    target_link_libraries(redisearch ${binroot}/redisearch_rs/libredisearch_rs.a)

    extract_debug_symbols(redisearch)

    add_dependencies(redisearch VectorSimilarity)

endif()

if (${COORD_TYPE} STREQUAL "oss")
    add_compile_definitions(PRIVATE)
elseif (${COORD_TYPE} STREQUAL "rlec")
    add_compile_definitions(PRIVATE RS_CLUSTER_ENTERPRISE)
    # TODO: why calling it module and not redisearch???
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "module-enterprise")
else()
    message(FATAL_ERROR "Invalid TYPE (='${TYPE}'). Should be either 'oss' or 'rlec'")
endif()

#----------------------------------------------------------------------------------------------

if (BUILD_SEARCH_UNIT_TESTS)
    enable_testing()

    add_subdirectory(tests/cpptests/redismock)

    set(BUILD_GTEST ON CACHE BOOL "enable gtest" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "disable gmock" FORCE)

    add_subdirectory(deps/googletest)
    add_subdirectory(tests/cpptests)
    add_subdirectory(tests/ctests)
    add_subdirectory(tests/ctests/ext-example example_extension)
    add_subdirectory(tests/ctests/coord_tests)

endif()
