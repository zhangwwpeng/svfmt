cmake_minimum_required(VERSION 3.20)
project(svfmt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SVFMT_BUILD_TESTS "Build svfmt tests" ON)
set(SVFMT_VERIBLE_FORMAT "verible-verilog-format" CACHE STRING
    "verible-verilog-format used as the oracle in differential tests (use the release matching the ported Verible commit)")
set(SVFMT_CORPUS_DIR "" CACHE PATH "Optional directory of SystemVerilog files for the corpus test")

# --- Dependencies -----------------------------------------------------------

# slang is consumed as a source dependency (git submodule). We only need the
# core library: no slang tools, tests or docs.
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/slang EXCLUDE_FROM_ALL)

# Abseil: the ported Verible code uses it throughout. Prefer an installed copy.
find_package(absl CONFIG QUIET)
if(NOT absl_FOUND)
  include(FetchContent)
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_BUILD_TESTING OFF)
  FetchContent_Declare(absl
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20250814.1
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(absl)
endif()

# --- Ported Verible formatter (src/verible, namespace verible) ---------------

file(GLOB_RECURSE VERIBLE_SOURCES CONFIGURE_DEPENDS src/verible/*.cc)
file(GLOB_RECURSE SVFMT_SOURCES CONFIGURE_DEPENDS src/svfmt/*.cc)

# Whitespace-only-difference check (raw lexer): run by svfmt on its own output
# and by the svfmt-lexcmp tool of the corpus tests.
add_library(svfmt_lexeq STATIC src/lexeq/lex-equivalence.cc)
target_include_directories(svfmt_lexeq PUBLIC src)
target_link_libraries(svfmt_lexeq PUBLIC slang::slang)

add_library(svfmt_core STATIC ${VERIBLE_SOURCES} ${SVFMT_SOURCES})
target_include_directories(svfmt_core PUBLIC src)
target_link_libraries(svfmt_core PUBLIC
  svfmt_lexeq
  slang::slang
  absl::check absl::die_if_null absl::log absl::status absl::statusor
  absl::strings absl::str_format absl::flags absl::flags_parse absl::flags_usage
  absl::node_hash_map absl::flat_hash_map absl::fixed_array absl::time absl::random_random
  absl::failure_signal_handler absl::symbolize absl::log_initialize absl::log_globals)

add_executable(svfmt src/main.cpp)
target_link_libraries(svfmt PRIVATE svfmt_core absl::log_initialize)

# Whitespace-only-difference checker used by the corpus tests.
add_executable(svfmt-lexcmp tools/lexcmp.cpp)
target_link_libraries(svfmt-lexcmp PRIVATE svfmt_lexeq)

# Preprocessed-token-stream comparison used by tests/run_preproc_diff.py.
add_executable(svfmt-preproc-cmp tools/preproc-cmp.cpp)
target_link_libraries(svfmt-preproc-cmp PRIVATE slang::slang)

if(SVFMT_BUILD_TESTS)
  enable_testing()
  add_executable(svfmt-format-range-test tests/format_range_regression.cc)
  target_link_libraries(svfmt-format-range-test PRIVATE svfmt_core)
  add_test(NAME format_range COMMAND svfmt-format-range-test)
  add_test(NAME format_inputs
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_format_inputs.sh $<TARGET_FILE:svfmt>
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/inputs
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/expected)
endif()

if(SVFMT_BUILD_TESTS)
  # Corpus safety test (no comparison with Verible).
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    # Differential preprocessor check and mutation test of the safety checks
    # on the regression inputs (fast).
    add_test(NAME preproc_diff
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_preproc_diff.py
                     ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/inputs)
    add_test(NAME mutation
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mutation.py
                     ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/inputs
                     --mutations 6 --format)
    add_test(NAME cli_regression
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli_regression.py
                     $<TARGET_FILE:svfmt>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_format_inputs.sh)
  endif()
  if(Python3_FOUND AND SVFMT_CORPUS_DIR)
    add_test(NAME corpus
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_corpus.py
                     ${CMAKE_BINARY_DIR} ${SVFMT_CORPUS_DIR} --no-parity
                     --out ${CMAKE_BINARY_DIR}/corpus_report)
  endif()
endif()
