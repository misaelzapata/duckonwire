cmake_minimum_required(VERSION 3.24)
project(DuckOnWire VERSION 0.3.0 LANGUAGES C CXX)

if(NOT UNIX)
  message(FATAL_ERROR "DuckOnWire's server currently requires POSIX (macOS or Linux).")
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C++ flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C flags" FORCE)
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()

include(cmake/Pins.cmake)
include(cmake/Bootstrap.cmake)
set(DUCKONWIRE_UPSTREAM_DIR "${CMAKE_SOURCE_DIR}/upstream" CACHE PATH
    "Clean, pinned fork/official Git checkouts; never patched or reset")
set(DUCKONWIRE_PATCHES_DIR "${CMAKE_SOURCE_DIR}/patches" CACHE PATH
    "Audit patches against official baselines; fork commits already include them")
duckonwire_bootstrap()

# Register the crypto/HTTP provider before Quack. Both are linked into DuckDB,
# so native clients use CONNECT without LOAD, INSTALL or unsigned extensions.
configure_file(cmake/Extensions.cmake.in "${CMAKE_BINARY_DIR}/extensions.cmake" @ONLY)
set(DUCKDB_EXTENSION_CONFIGS "${CMAKE_BINARY_DIR}/extensions.cmake" CACHE STRING "" FORCE)
set(BUILD_EXTENSIONS "core_functions;json;parquet;icu" CACHE STRING "" FORCE)
set(GIT_COMMIT_HASH "${DUCKONWIRE_DUCKDB_COMMIT}" CACHE STRING "" FORCE)
set(DUCKDB_EXPLICIT_VERSION "v2.1.0-dev0" CACHE STRING "" FORCE)
set(ENABLE_EXTENSION_AUTOINSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOLOADING OFF CACHE BOOL "" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

# Callers can supply OPENSSL_ROOT_DIR. HTTPFS also needs system libcurl headers.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(candidate "/opt/homebrew/opt/openssl@3" "$ENV{HOME}/brew/opt/openssl@3"
                    "/usr/local/opt/openssl@3")
    if(EXISTS "${candidate}/include/openssl/ssl.h")
      set(OPENSSL_ROOT_DIR "${candidate}" CACHE PATH "OpenSSL prefix")
      break()
    endif()
  endforeach()
endif()
if(APPLE)
  set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Use static OpenSSL")
endif()
add_subdirectory("${DUCKONWIRE_DUCKDB_SOURCE}" "${CMAKE_BINARY_DIR}/duckdb")
if(TARGET clangd_cache)
  # Upstream assumes a build/<type> directory when deriving its cache path.
  # Our root build/ keeps compiler metadata local and skips that extra copy.
  set_property(TARGET clangd_cache PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
add_library(DuckDB::DuckDB ALIAS duckdb)
set_target_properties(duckdb PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
target_include_directories(duckdb INTERFACE
  "$<BUILD_INTERFACE:${DUCKONWIRE_DUCKDB_SOURCE}/src/include>"
  "$<INSTALL_INTERFACE:include>")

if(APPLE)
  set(DUCKONWIRE_INSTALL_RPATH "@loader_path/../lib")
else()
  set(DUCKONWIRE_INSTALL_RPATH "$ORIGIN/../lib")
endif()
function(duckonwire_executable target source)
  add_executable(${target} "${source}")
  target_link_libraries(${target} PRIVATE DuckDB::DuckDB)
  target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/include")
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    INSTALL_RPATH "${DUCKONWIRE_INSTALL_RPATH}")
endfunction()
find_package(Threads REQUIRED)
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS SSL Crypto)
add_library(duckonwire-auth STATIC src/scram.cpp src/access.cpp)
target_include_directories(duckonwire-auth PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(duckonwire-auth PUBLIC OpenSSL::Crypto)
add_library(duckonwire-pgwire STATIC src/pgwire.cpp)
target_include_directories(duckonwire-pgwire PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(duckonwire-pgwire PUBLIC DuckDB::DuckDB Threads::Threads duckonwire-auth OpenSSL::SSL)
add_executable(duckonwire-password src/password.cpp)
target_link_libraries(duckonwire-password PRIVATE duckonwire-auth)
set_target_properties(duckonwire-password PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
duckonwire_executable(duckonwire-server src/server.cpp)
target_sources(duckonwire-server PRIVATE examples/demo/initialize.cpp)
target_include_directories(duckonwire-server PRIVATE "${CMAKE_SOURCE_DIR}/examples/demo")
target_link_libraries(duckonwire-server PRIVATE duckonwire-pgwire)
duckonwire_executable(cpp-crud-client examples/crud_cpp.cpp)
duckonwire_executable(c-crud-client examples/crud_c.c)
duckonwire_executable(transaction-flow examples/transaction_flow.cpp)
duckonwire_executable(attach-client examples/attach.cpp)
install(TARGETS duckonwire-server duckonwire-password cpp-crud-client c-crud-client transaction-flow attach-client
        RUNTIME DESTINATION bin)
install(DIRECTORY examples/ DESTINATION examples
  PATTERN node_modules EXCLUDE PATTERN target EXCLUDE PATTERN obj EXCLUDE
  PATTERN bin EXCLUDE PATTERN __pycache__ EXCLUDE
  PATTERN "*.duckdb*" EXCLUDE PATTERN "*.wal" EXCLUDE)
install(FILES LICENSE THIRD_PARTY_NOTICES.txt DESTINATION share/duckonwire)

include(CTest)
if(BUILD_TESTING)
  duckonwire_executable(native-tests tests/native_tests.cpp)
  duckonwire_executable(attach-tests tests/attach_tests.cpp)
  duckonwire_executable(pgwire-protocol-tests tests/pgwire_protocol_tests.cpp)
  target_link_libraries(pgwire-protocol-tests PRIVATE duckonwire-auth OpenSSL::SSL)
  duckonwire_executable(database-routing-tests tests/database_routing_tests.cpp)
  target_link_libraries(database-routing-tests PRIVATE duckonwire-pgwire)
  add_executable(access-tests tests/access_tests.cpp)
  target_link_libraries(access-tests PRIVATE duckonwire-auth)
  set_target_properties(access-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_executable(scram-tests tests/scram_tests.cpp)
  target_link_libraries(scram-tests PRIVATE duckonwire-auth)
  set_target_properties(scram-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  duckonwire_executable(duckonwire-benchmark tests/concurrent_benchmark.cpp)
  target_include_directories(native-tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  target_include_directories(attach-tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  target_include_directories(pgwire-protocol-tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  target_include_directories(duckonwire-benchmark PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
  install(TARGETS native-tests attach-tests pgwire-protocol-tests database-routing-tests access-tests scram-tests duckonwire-benchmark RUNTIME DESTINATION bin
          COMPONENT Tests EXCLUDE_FROM_ALL)
  add_dependencies(native-tests duckonwire-server transaction-flow)
  add_dependencies(attach-tests duckonwire-server attach-client)
  add_dependencies(pgwire-protocol-tests duckonwire-server)
  add_dependencies(duckonwire-benchmark duckonwire-server)
  add_test(NAME native-tests COMMAND native-tests
    --server "$<TARGET_FILE:duckonwire-server>" --flow "$<TARGET_FILE:transaction-flow>")
  add_test(NAME native-attach COMMAND attach-tests
    --server "$<TARGET_FILE:duckonwire-server>" --client "$<TARGET_FILE:attach-client>"
    --fixture-parent "${CMAKE_BINARY_DIR}/test-results")
  add_test(NAME pgwire-protocol COMMAND pgwire-protocol-tests
    --server "$<TARGET_FILE:duckonwire-server>"
    --fixture-parent "${CMAKE_BINARY_DIR}/test-results")
  add_test(NAME access-rules COMMAND access-tests --fixture-parent "${CMAKE_BINARY_DIR}/test-results")
  add_test(NAME scram-authentication COMMAND scram-tests)
  add_test(NAME database-routing COMMAND database-routing-tests)
  add_test(NAME concurrent-smoke COMMAND duckonwire-benchmark
    --rows 10000 --batch 250 --mode both --server "$<TARGET_FILE:duckonwire-server>"
    --output "${CMAKE_BINARY_DIR}/test-results/concurrent-smoke.json")
  set_tests_properties(native-tests native-attach pgwire-protocol concurrent-smoke PROPERTIES TIMEOUT 120)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-results")
  set_tests_properties(native-tests native-attach pgwire-protocol concurrent-smoke access-rules PROPERTIES
    ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-results")
  add_test(NAME bootstrap-regressions COMMAND "${CMAKE_COMMAND}"
    "-DTEST_ROOT=${CMAKE_BINARY_DIR}/bootstrap-test-fixture"
    -P "${CMAKE_SOURCE_DIR}/cmake/BootstrapTests.cmake")
  set_tests_properties(bootstrap-regressions PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
message(STATUS "DuckOnWire: native DuckDB v2.1.0-dev0, Quack protocol 7")
message(STATUS "Clean upstream: ${DUCKONWIRE_UPSTREAM_DIR}")
message(STATUS "Patched working checkouts: ${CMAKE_BINARY_DIR}/sources")
