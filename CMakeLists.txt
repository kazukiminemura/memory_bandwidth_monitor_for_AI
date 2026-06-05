cmake_minimum_required(VERSION 3.20)

project(memory_bandwidth_monitor_for_AI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mbm STATIC
    src/mbm_profiler.cpp
)

target_include_directories(mbm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mbm_cli
    src/main.cpp
)

target_link_libraries(mbm_cli PRIVATE mbm)

if(WIN32)
    target_link_libraries(mbm_cli PRIVATE psapi)
endif()
