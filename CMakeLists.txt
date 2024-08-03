cmake_minimum_required(VERSION 3.27)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Do not build in-source. Please remove CMakeCache.txt and the CMakeFiles/ directory. Then build out-of-source.")
endif()

project(sparsemap LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_OUTPUT_EXTENSION .o)

set(SOURCE_DIR .)
set(HEADER_DIR . test)

set(COMMON_CMAKE_C_FLAGS "-std=c11 -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_DEBUG "-DSPARSEMAP_DIAGNOSTIC -DSPARSEMAP_TESTING -DDEBUG -g -Og")
set(CMAKE_C_FLAGS_PROFILE "-DSPARSEMAP_DIAGNOSTIC -DSPARSEMAP_TESTING -DDEBUG -g -Og -fsanitize=address,leak,object-size,pointer-compare,pointer-subtract,null,return,bounds,pointer-overflow,undefined -fsanitize-address-use-after-scope")
set(CMAKE_C_FLAGS_RELEASE "-Ofast")

# Include all header files from the header directory
file(GLOB_RECURSE HEADERS CONFIGURE_FILES ${HEADER_DIR}/*.h)

# Configure library sources
set(LIB_SRC
    ${SOURCE_DIR}/sparsemap.c
)

# Option to control building shared/static libraries
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# Add shared library
add_library(sparsemap_SHARED SHARED ${LIB_SRC} ${HEADERS})
# Set target properties for the shared library (adjust if needed)
set_target_properties(sparsemap_SHARED PROPERTIES
    VERSION 1.0.0  # Set library version
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"  # Set output directory
    COMPILE_FLAGS "${CMAKE_C_FLAGS_${CMAKE_CURRENT_LIST_MODE}}"
)
target_include_directories(sparsemap_SHARED PRIVATE ${HEADER_DIR})

# Add static library
add_library(sparsemap STATIC ${LIB_SRC} ${HEADERS})
# Set target properties for static library (adjust if needed)
set_target_properties(sparsemap PROPERTIES
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"  # Set output directory
)
target_include_directories(sparsemap PRIVATE ${HEADER_DIR})

# Add ex_1 program
add_executable(ex_1 test/ex_1.c test/munit.c test/qc.c test/common.c)
target_link_libraries(ex_1 PRIVATE sparsemap)
target_include_directories(ex_1 PRIVATE ${HEADER_DIR})
add_custom_target(run_ex_1 COMMAND ex_1 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add ex_2 program
add_executable(ex_2 test/ex_2.c test/munit.c test/qc.c test/common.c)
target_link_libraries(ex_2 PRIVATE sparsemap)
target_include_directories(ex_2 PRIVATE ${HEADER_DIR})
add_custom_target(run_ex_2 COMMAND ex_2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add ex_3 program
add_executable(ex_3 test/ex_3.c test/munit.c test/qc.c test/common.c)
target_link_libraries(ex_3 PRIVATE sparsemap)
target_include_directories(ex_3 PRIVATE ${HEADER_DIR})
add_custom_target(run_ex_3 COMMAND ex_3 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add ex_4 program
add_executable(ex_4 test/ex_4.c test/munit.c test/qc.c test/common.c)
target_link_libraries(ex_4 PRIVATE sparsemap)
target_include_directories(ex_4 PRIVATE ${HEADER_DIR})
add_custom_target(run_ex_4 COMMAND ex_4 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add test program
add_executable(test test/test.c test/munit.c test/qc.c test/common.c)
target_link_libraries(test PRIVATE sparsemap)
target_include_directories(test PRIVATE ${HEADER_DIR})
set_source_files_properties(test/test.c PROPERTIES COMPILE_FLAGS "-DDEBUG -DSPARSEMAP_DIAGNOSTIC -DSPARSEMAP_TESTING" )
add_custom_target(run_test COMMAND test WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add soak program
add_executable(soak test/soak.c test/common.c test/tdigest.c test/qc.c test/roaring.c)
target_link_libraries(soak PRIVATE sparsemap)
target_include_directories(soak PRIVATE ${HEADER_DIR} lib)
target_link_libraries(soak PUBLIC m)
add_custom_target(run_soak COMMAND soak WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Add fuzzer program
# add_executable(fuzzer EXCLUDE_FROM_ALL tests/fuzzer.c)
# target_link_libraries(fuzzer PRIVATE sparsemap)
# target_include_directories(fuzzer PRIVATE ${HEADER_DIR} lib)
# target_link_libraries(fuzzer PUBLIC m)
# add_custom_target(run_fuzzer COMMAND fuzzer WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
