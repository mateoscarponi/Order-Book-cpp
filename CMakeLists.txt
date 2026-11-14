cmake_minimum_required(VERSION 3.16)
project(orderbook_cpp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_WERROR "Treat warnings as errors" OFF)
set(OB_SANITIZE "" CACHE STRING "Sanitizer build: empty, 'address' (ASan + UBSan) or 'thread' (TSan)")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
  if(OB_WERROR)
    add_compile_options(-Werror)
  endif()
  if(OB_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(OB_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  endif()
endif()

# On Windows (MinGW), link the C++ runtime statically so the programs run outside the compiler's shell.
if(MINGW)
  add_link_options(-static)
endif()

find_package(Threads REQUIRED)

add_library(orderbook
  src/order_book.cpp
  src/matching_engine.cpp
)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(orderbook PUBLIC Threads::Threads)

add_executable(replay apps/replay.cpp)
target_link_libraries(replay PRIVATE orderbook)

add_executable(bench apps/bench.cpp)
target_link_libraries(bench PRIVATE orderbook)

# Line-based front end used by the web demo in demo/
add_executable(interactive apps/interactive.cpp)
target_link_libraries(interactive PRIVATE orderbook)

enable_testing()
add_executable(unit_tests tests/tests.cpp)
target_link_libraries(unit_tests PRIVATE orderbook)
add_test(NAME unit_tests COMMAND unit_tests)
add_test(NAME replay_example COMMAND replay ${CMAKE_CURRENT_SOURCE_DIR}/examples/orders.csv)
