# The new nvsound core on its own, for its unit tests. NVGT builds these sources with SCons, and Golden Crayon 2.0 with nanobind.
cmake_minimum_required(VERSION 3.20)
project(nvsound_core CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
	# C4324 is the padding alignas(64) asks for on purpose, so the two ends of a ring never share a cache line.
	add_compile_options(/W4 /utf-8 /permissive- /wd4324)
endif()

add_library(nvsound_core STATIC realtime.cpp)
target_include_directories(nvsound_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
add_executable(test_realtime tests/test_realtime.cpp)
target_link_libraries(test_realtime nvsound_core)
add_test(NAME realtime COMMAND test_realtime)
