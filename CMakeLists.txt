cmake_minimum_required(VERSION 3.22)
project(AIContact VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")
endif()

option(AICONTACT_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/Standalone)" ON)
option(AICONTACT_BUILD_HOST "Build the headless aicontact-host tool" ON)

include(FetchContent)
# Offline builds: -DFETCHCONTENT_SOURCE_DIR_WASM3=/path/to/wasm3 (likewise JUCE, JSON).

FetchContent_Declare(wasm3
    GIT_REPOSITORY https://github.com/wasm3/wasm3.git
    GIT_TAG 28ecb9af6d2040e474a70f7cb7f43666740141fb
    SOURCE_SUBDIR source)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(wasm3 json)

if(MSVC)
    # wasm3 relies on GNU-style ", ##__VA_ARGS__" comma elision, which only MSVC's
    # conforming preprocessor understands.
    target_compile_options(m3 PRIVATE /w /Zc:preprocessor)
else()
    target_compile_options(m3 PRIVATE -w -O2)
endif()

add_library(aicontact_core STATIC
    core/src/Util.cpp
    core/src/Log.cpp
    core/src/WasmInfo.cpp
    core/src/Instrument.cpp
    core/src/Rack.cpp
    core/src/Analyze.cpp
    core/src/ControlServer.cpp
    core/src/Auth.cpp
    core/src/Ui.cpp)
target_include_directories(aicontact_core PUBLIC core/include)
target_link_libraries(aicontact_core PUBLIC nlohmann_json::nlohmann_json PRIVATE m3)
if(WIN32)
    target_link_libraries(aicontact_core PRIVATE ws2_32 bcrypt)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(aicontact_core PUBLIC Threads::Threads)
endif()

if(AICONTACT_BUILD_HOST)
    add_executable(aicontact-host host/aicontact_host.cpp)
    target_link_libraries(aicontact-host PRIVATE aicontact_core)
endif()

if(AICONTACT_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

option(AICONTACT_BUILD_PLUGIN_TEST "Build the VST3 hosting test (needs the plugin)" OFF)
if(AICONTACT_BUILD_PLUGIN AND AICONTACT_BUILD_PLUGIN_TEST)
    add_subdirectory(tests/plugin_host)
endif()
