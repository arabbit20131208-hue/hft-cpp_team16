# ─────────────────────────────────────────────────────────────────────────────
# AlgoArena — Team 16 HFT client, Phase 4: gateway + strategy processes
#
#   hft_bot    the Gateway process: owns the WebSocket (or --replay stdin/stdout),
#              starts and supervises `strategy`, talks to it over shared memory
#   strategy   the Strategy process: local book, signal, queue-aware quoting
#
# Shared headers from the repo root include/: spsc_ring.hpp, shm_ring.hpp.
# Dependencies via FetchContent: IXWebSocket, nlohmann/json.
#
# Build:  cmake -S hft/cpp_client -B build && cmake --build build -j
# Run:    hft/cpp_client/launch.sh            (live, see README.md)
#         ./build/hft_bot --replay < tape     (offline; starts ./build/strategy itself)
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.16)
project(algoarena_hft_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HFT_USE_TLS "Build IXWebSocket with TLS support (needed for wss://)" OFF)
option(HFT_TSAN "Build everything with ThreadSanitizer (-fsanitize=thread)" OFF)

if(HFT_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
if(HFT_USE_TLS)
  if(APPLE)
    set(USE_SECURE_TRANSPORT ON CACHE BOOL "" FORCE)
  else()
    set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  endif()
  set(USE_TLS ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json ixwebsocket)
find_package(Threads REQUIRED)

if(MSVC)
  set(HFT_WARNINGS /W4)
else()
  set(HFT_WARNINGS -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 12/13 false positive: it attributes writes into a ring slot (buf_[i & mask])
  # to the neighbouring atomic counter and reports a 0-byte region.
  list(APPEND HFT_WARNINGS -Wno-stringop-overflow)
endif()

set(HFT_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
# shm_open lives in librt on older glibc; harmless elsewhere.
find_library(RT_LIBRARY rt)
set(HFT_SHM_LIBS Threads::Threads)
if(RT_LIBRARY)
  list(APPEND HFT_SHM_LIBS ${RT_LIBRARY})
endif()

# ── the two processes ───────────────────────────────────────────────────────
add_executable(hft_bot src/gateway.cpp)
target_include_directories(hft_bot PRIVATE ${HFT_INCLUDES})
target_link_libraries(hft_bot PRIVATE ixwebsocket nlohmann_json::nlohmann_json ${HFT_SHM_LIBS})
target_compile_options(hft_bot PRIVATE ${HFT_WARNINGS})

add_executable(strategy src/strategy.cpp)
target_include_directories(strategy PRIVATE ${HFT_INCLUDES})
target_link_libraries(strategy PRIVATE ${HFT_SHM_LIBS})
target_compile_options(strategy PRIVATE ${HFT_WARNINGS})
add_dependencies(hft_bot strategy)  # the gateway starts ./strategy

# ── tests, benchmarks, tools ────────────────────────────────────────────────
add_executable(book_test tests/book_test.cpp)
target_include_directories(book_test PRIVATE ${HFT_INCLUDES})
target_link_libraries(book_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(book_test PRIVATE ${HFT_WARNINGS} $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_executable(shm_test tests/shm_test.cpp)
target_include_directories(shm_test PRIVATE ${HFT_INCLUDES})
target_link_libraries(shm_test PRIVATE ${HFT_SHM_LIBS})
target_compile_options(shm_test PRIVATE ${HFT_WARNINGS} $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_executable(book_bench tests/book_bench.cpp)
target_include_directories(book_bench PRIVATE ${HFT_INCLUDES})
target_compile_options(book_bench PRIVATE ${HFT_WARNINGS})

add_executable(shm_ipc_bench tools/shm_ipc_bench.cpp)
target_include_directories(shm_ipc_bench PRIVATE ${HFT_INCLUDES})
target_link_libraries(shm_ipc_bench PRIVATE ${HFT_SHM_LIBS})
target_compile_options(shm_ipc_bench PRIVATE ${HFT_WARNINGS})

add_executable(tape_driver tools/tape_driver.cpp)
target_link_libraries(tape_driver PRIVATE Threads::Threads)
target_compile_options(tape_driver PRIVATE ${HFT_WARNINGS})

enable_testing()
add_test(NAME local_book COMMAND book_test)
add_test(NAME shm COMMAND shm_test)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME replay_two_process
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_test.py
                   $<TARGET_FILE:hft_bot>)
  add_test(NAME strategy_crash_recovery
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/resilience_test.py
                   $<TARGET_FILE:hft_bot>)
endif()
