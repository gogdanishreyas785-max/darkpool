cmake_minimum_required(VERSION 3.20)
project(darkpool_s1 LANGUAGES CXX)

option(DARKPOOL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(nlohmann_json 3.11.3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(darkpool_engine src/game_engine.cpp)
target_include_directories(darkpool_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_engine PUBLIC cxx_std_20)

add_library(darkpool_network src/network.cpp)
target_include_directories(darkpool_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(darkpool_network PUBLIC cxx_std_20)
target_link_libraries(darkpool_network PUBLIC nlohmann_json::nlohmann_json)

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

enable_testing()
add_executable(darkpool_engine_tests tests/engine_tests.cpp)
target_link_libraries(darkpool_engine_tests PRIVATE darkpool_engine)
darkpool_enable_warnings(darkpool_engine_tests)
add_test(NAME darkpool_engine_tests COMMAND darkpool_engine_tests)

add_executable(darkpool_network_tests tests/network_tests.cpp)
target_link_libraries(darkpool_network_tests PRIVATE darkpool_network)
darkpool_enable_warnings(darkpool_network_tests)
add_test(NAME darkpool_network_tests COMMAND darkpool_network_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(darkpool_server src/server_main.cpp)
  target_link_libraries(darkpool_server PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_server)

  add_executable(darkpool_client src/client_main.cpp)
  target_link_libraries(darkpool_client PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_client)

  add_executable(darkpool_network_integration_tests tests/network_integration_tests.cpp)
  target_link_libraries(darkpool_network_integration_tests PRIVATE darkpool_network)
  darkpool_enable_warnings(darkpool_network_integration_tests)
  add_test(NAME darkpool_network_integration_tests
    COMMAND darkpool_network_integration_tests $<TARGET_FILE:darkpool_server>)
else()
  message(STATUS "TCP server and CLI client require Linux; engine and framing tests remain available")
endif()
