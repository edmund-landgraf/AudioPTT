cmake_minimum_required(VERSION 3.21)
project(AudioPTT LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(AudioPTT WIN32
    src/main.cpp
    src/AudioDevices.cpp
    src/AudioEngine.cpp
    src/AudioRouting.cpp
    src/AudioRouteStore.cpp
    src/Diagnostics.cpp
    src/AudioEchoTest.cpp
    src/AudioSessionDiagnostics.cpp
)
target_compile_definitions(AudioPTT PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(AudioPTT PRIVATE ole32 uuid advapi32 comctl32)
if(MSVC)
    target_compile_options(AudioPTT PRIVATE /W4 /permissive-)
endif()
