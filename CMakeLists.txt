cmake_minimum_required(VERSION 3.20)
project(svfmt LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/slang EXCLUDE_FROM_ALL)

include_directories(
    include
    third_party/slang/include
    ${CMAKE_CURRENT_BINARY_DIR}/third_party/slang/source
)

add_library(svfmt_lib STATIC
    src/FormatStyle.cpp
    src/CommentControls.cpp
    src/TokenAnnotator.cpp
    src/Alignment.cpp
    src/Formatter.cpp
)

target_include_directories(svfmt_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(svfmt_lib PUBLIC slang::slang)

add_executable(svfmt src/Main.cpp)
target_link_libraries(svfmt PRIVATE svfmt_lib)

enable_testing()
add_executable(svfmt_test tests/FormatterTest.cpp)
target_link_libraries(svfmt_test PRIVATE svfmt_lib)
add_test(NAME svfmt_test COMMAND svfmt_test)
