cmake_minimum_required(VERSION 3.25)
project(tlou1-coop LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include(FetchContent)
set(SAFETYHOOK_FETCH_ZYDIS ON)
FetchContent_Declare(safetyhook
    GIT_REPOSITORY https://github.com/cursey/safetyhook
    GIT_TAG        629558c64009a7291ba6ed5cfb49187086a27a47)   # тот же пин, что у TLOU2-Coop
FetchContent_MakeAvailable(safetyhook)

get_filename_component(LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(CMAKE_ASM_MASM_COMPILER NAMES llvm-ml64 ml64 HINTS "${LLVM_BIN}" REQUIRED)
enable_language(ASM_MASM)
add_library(winmm-thunks OBJECT src/winmm_thunks.asm)
add_library(tlou1-coop SHARED src/main.cpp src/winmm-proxy.cpp src/winmm_exports.def)
target_compile_options(tlou1-coop PRIVATE /W4)
target_link_libraries(tlou1-coop PRIVATE winmm-thunks safetyhook)
set_target_properties(tlou1-coop PROPERTIES OUTPUT_NAME winmm PREFIX "")
add_executable(smoke test/smoke.cpp)
