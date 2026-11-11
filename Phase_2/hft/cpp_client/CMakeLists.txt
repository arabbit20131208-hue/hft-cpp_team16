cmake_minimum_required(VERSION 3.16)
project(algoarena_hft_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HFT_USE_TLS "Build IXWebSocket with TLS support (needed for wss://)" OFF)

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

# ── The bot ─────────────────────────────────────────────────────────────────
add_executable(hft_bot src/main.cpp src/arena_client.cpp)
target_include_directories(hft_bot PRIVATE include)
target_link_libraries(hft_bot PRIVATE ixwebsocket nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(hft_bot PRIVATE ${HFT_WARNINGS})

# ── Tests and benchmark (all sources live inside this directory) ────────────
add_executable(book_test tests/book_test.cpp)
target_include_directories(book_test PRIVATE include)
target_link_libraries(book_test PRIVATE nlohmann_json::nlohmann_json)
# Keep assert() active in Release.
target_compile_options(book_test PRIVATE ${HFT_WARNINGS} $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)

add_executable(book_bench tests/book_bench.cpp)
target_include_directories(book_bench PRIVATE include)
target_compile_options(book_bench PRIVATE ${HFT_WARNINGS})

enable_testing()
add_test(NAME local_book COMMAND book_test)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME queue_replay
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_test.py
                   $<TARGET_FILE:hft_bot>)
endif()
