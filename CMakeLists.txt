cmake_minimum_required(VERSION 3.20)
project(darkpool_s1 LANGUAGES CXX)

option(DARKPOOL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(nlohmann_json 3.11.3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    FetchContent_Declare(nlohmann_json
      URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
      URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  else()
    FetchContent_Declare(nlohmann_json
      URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
      URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406)
  endif()
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(darkpool_engine src/game_engine.cpp)
target_include_directories(darkpool_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_engine PUBLIC cxx_std_20)

add_library(darkpool_network src/network.cpp)
target_include_directories(darkpool_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_network PUBLIC cxx_std_20)
target_link_libraries(darkpool_network PUBLIC nlohmann_json::nlohmann_json)

add_library(darkpool_coordinator src/match_coordinator.cpp)
target_include_directories(darkpool_coordinator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_coordinator PUBLIC cxx_std_20)
target_link_libraries(darkpool_coordinator PUBLIC darkpool_engine darkpool_network)

add_library(darkpool_cli src/cli.cpp)
target_include_directories(darkpool_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_cli PUBLIC cxx_std_20)
target_link_libraries(darkpool_cli PUBLIC darkpool_network)

function(darkpool_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(DARKPOOL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(DARKPOOL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

darkpool_enable_warnings(darkpool_engine)
darkpool_enable_warnings(darkpool_network)
darkpool_enable_warnings(darkpool_coordinator)
darkpool_enable_warnings(darkpool_cli)

enable_testing()
add_executable(darkpool_engine_tests tests/engine_tests.cpp)
target_link_libraries(darkpool_engine_tests PRIVATE darkpool_engine)
darkpool_enable_warnings(darkpool_engine_tests)
add_test(NAME darkpool_engine_tests COMMAND darkpool_engine_tests)

add_executable(darkpool_network_tests tests/network_tests.cpp)
target_link_libraries(darkpool_network_tests PRIVATE darkpool_network)
darkpool_enable_warnings(darkpool_network_tests)
add_test(NAME darkpool_network_tests COMMAND darkpool_network_tests)

add_executable(darkpool_coordinator_tests tests/match_coordinator_tests.cpp)
target_link_libraries(darkpool_coordinator_tests PRIVATE darkpool_coordinator)
darkpool_enable_warnings(darkpool_coordinator_tests)
add_test(NAME darkpool_coordinator_tests COMMAND darkpool_coordinator_tests)

add_executable(darkpool_cli_tests tests/cli_tests.cpp)
target_link_libraries(darkpool_cli_tests PRIVATE darkpool_cli)
darkpool_enable_warnings(darkpool_cli_tests)
add_test(NAME darkpool_cli_tests COMMAND darkpool_cli_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(darkpool_server src/server_main.cpp)
  target_link_libraries(darkpool_server PRIVATE darkpool_coordinator)
  darkpool_enable_warnings(darkpool_server)

  add_executable(darkpool_client src/client_main.cpp)
  target_link_libraries(darkpool_client PRIVATE darkpool_cli)
  darkpool_enable_warnings(darkpool_client)

  add_executable(darkpool_network_integration_tests tests/network_integration_tests.cpp)
  target_link_libraries(darkpool_network_integration_tests PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_network_integration_tests)
  add_test(NAME darkpool_network_integration_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server>)
  add_test(NAME darkpool_timeout_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> timeout)
  set_tests_properties(darkpool_timeout_tests PROPERTIES TIMEOUT 20)
  add_test(NAME darkpool_auth_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> auth)
  add_test(NAME darkpool_gameplay_e2e_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> gameplay)
  add_test(NAME darkpool_variable_gameplay_e2e_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> variable-gameplay)
  add_test(NAME darkpool_malformed_tcp_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> malformed)
  set_tests_properties(darkpool_malformed_tcp_tests PROPERTIES TIMEOUT 30)
  add_test(NAME darkpool_unix_socket_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server> unix)
  set_tests_properties(darkpool_unix_socket_tests PROPERTIES TIMEOUT 20)

  add_executable(darkpool_load_tests tests/load_test_harness.cpp)
  target_link_libraries(darkpool_load_tests PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_load_tests)
  execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DARKPOOL_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(DARKPOOL_GIT_COMMIT)
    target_compile_definitions(darkpool_load_tests PRIVATE DARKPOOL_SOURCE_COMMIT="${DARKPOOL_GIT_COMMIT}")
  endif()
  add_test(NAME darkpool_load_tests
    COMMAND darkpool_load_tests $<TARGET_FILE:darkpool_server>)
  set_tests_properties(darkpool_load_tests PROPERTIES TIMEOUT 30)
  add_test(NAME darkpool_load_regression_tests
    COMMAND darkpool_load_tests $<TARGET_FILE:darkpool_server> --selftest)
  set_tests_properties(darkpool_load_regression_tests PROPERTIES TIMEOUT 30)
  add_executable(darkpool_load_tests_64p tests/load_test_64p_harness.cpp)
  target_link_libraries(darkpool_load_tests_64p PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_load_tests_64p)
  if(DARKPOOL_GIT_COMMIT)
    target_compile_definitions(darkpool_load_tests_64p PRIVATE DARKPOOL_SOURCE_COMMIT="${DARKPOOL_GIT_COMMIT}")
  endif()
  add_test(NAME darkpool_load_64p_functional
    COMMAND darkpool_load_tests_64p $<TARGET_FILE:darkpool_server> --functional)
  set_tests_properties(darkpool_load_64p_functional PROPERTIES TIMEOUT 60)
  add_test(NAME darkpool_load_64p_regression
    COMMAND darkpool_load_tests_64p $<TARGET_FILE:darkpool_server> --selftest)
  set_tests_properties(darkpool_load_64p_regression PROPERTIES TIMEOUT 60)
  add_test(NAME darkpool_load_64p_semantic_regression
    COMMAND darkpool_load_tests_64p $<TARGET_FILE:darkpool_server> --test-semantic-response)
  add_executable(darkpool_load_tests_l5 tests/load_test_l5_harness.cpp)
  target_link_libraries(darkpool_load_tests_l5 PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_load_tests_l5)
  if(DARKPOOL_GIT_COMMIT)
    target_compile_definitions(darkpool_load_tests_l5 PRIVATE DARKPOOL_SOURCE_COMMIT="${DARKPOOL_GIT_COMMIT}")
  endif()
  add_test(NAME darkpool_load_l5_functional
    COMMAND darkpool_load_tests_l5 $<TARGET_FILE:darkpool_server> --functional --out docs/load_test_l5_results.json)
  set_tests_properties(darkpool_load_l5_functional PROPERTIES TIMEOUT 60)
  add_test(NAME darkpool_load_l5_regression
    COMMAND darkpool_load_tests_l5 $<TARGET_FILE:darkpool_server> --selftest)
  set_tests_properties(darkpool_load_l5_regression PROPERTIES TIMEOUT 60)

  add_executable(darkpool_load_tests_l6 tests/load_test_l6_harness.cpp)
  target_link_libraries(darkpool_load_tests_l6 PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_load_tests_l6)
  if(DARKPOOL_GIT_COMMIT)
    target_compile_definitions(darkpool_load_tests_l6 PRIVATE DARKPOOL_SOURCE_COMMIT="${DARKPOOL_GIT_COMMIT}")
  endif()
  add_test(NAME darkpool_load_l6_functional
    COMMAND darkpool_load_tests_l6 $<TARGET_FILE:darkpool_server> --functional)
  set_tests_properties(darkpool_load_l6_functional PROPERTIES TIMEOUT 60)
  add_test(NAME darkpool_load_l6_regression
    COMMAND darkpool_load_tests_l6 $<TARGET_FILE:darkpool_server> --selftest)
  set_tests_properties(darkpool_load_l6_regression PROPERTIES TIMEOUT 60)
  add_test(NAME darkpool_load_l6_sustained_short
    COMMAND darkpool_load_tests_l6 $<TARGET_FILE:darkpool_server> --test-sustained-short)
  set_tests_properties(darkpool_load_l6_sustained_short PROPERTIES TIMEOUT 60)

  option(DARKPOOL_ENABLE_SUSTAINED_TESTS "Enable 10-minute and 30-minute sustained load tests in CTest" OFF)
  if(DARKPOOL_ENABLE_SUSTAINED_TESTS)
    add_test(NAME darkpool_load_64p_smoke_10m
      COMMAND darkpool_load_tests_64p $<TARGET_FILE:darkpool_server> --smoke-10m)
    set_tests_properties(darkpool_load_64p_smoke_10m PROPERTIES TIMEOUT 700 LABELS "Sustained")
    add_test(NAME darkpool_load_64p_reliability_30m
      COMMAND darkpool_load_tests_64p $<TARGET_FILE:darkpool_server> --reliability-30m)
    set_tests_properties(darkpool_load_64p_reliability_30m PROPERTIES TIMEOUT 2000 LABELS "Sustained")
    add_test(NAME darkpool_load_l6_smoke_10m
      COMMAND darkpool_load_tests_l6 $<TARGET_FILE:darkpool_server> --smoke-10m)
    set_tests_properties(darkpool_load_l6_smoke_10m PROPERTIES TIMEOUT 700 LABELS "Sustained;L6")
    add_test(NAME darkpool_load_l6_reliability_30m
      COMMAND darkpool_load_tests_l6 $<TARGET_FILE:darkpool_server> --reliability-30m)
    set_tests_properties(darkpool_load_l6_reliability_30m PROPERTIES TIMEOUT 2000 LABELS "Sustained;L6")
  endif()
else()
  message(STATUS "TCP/UNIX server and CLI client require Linux; engine and framing tests remain available")
endif()
