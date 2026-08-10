# Builds Minima's portable C++ core (src/core/) + llama.cpp (vendor/llama.cpp) into
# libminima_core.so for the Android shell — the same core headers and the same on-device
# AI engine family the desktop builds use, reached over JNI.
cmake_minimum_required(VERSION 3.22)
project(minima_core_android CXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

# llama.cpp (CPU-only). Non-standalone: tests/tools/examples/server default OFF.
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPO_ROOT}/vendor/llama.cpp llama.cpp-build)

add_library(minima_core SHARED minima_jni.cpp)
target_compile_features(minima_core PRIVATE cxx_std_17)
target_link_libraries(minima_core PRIVATE llama)
