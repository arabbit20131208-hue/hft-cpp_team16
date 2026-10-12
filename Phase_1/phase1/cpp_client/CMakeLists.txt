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

option(PHASE1_FAST "Use the Phase 1 hot path" ON)

add_executable(hft_bot
  src/main.cpp
  src/arena_client.cpp
)

target_compile_definitions(hft_bot PRIVATE PHASE1_FAST=$<BOOL:${PHASE1_FAST}>)

target_include_directories(hft_bot PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hft_bot PRIVATE
  ixwebsocket
  nlohmann_json::nlohmann_json
)

find_package(Threads REQUIRED)
target_link_libraries(hft_bot PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(hft_bot PRIVATE /W4)
else()
  target_compile_options(hft_bot PRIVATE -Wall -Wextra)
endif()
