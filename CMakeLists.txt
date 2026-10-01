cmake_minimum_required(VERSION 3.25)

project(LoadAccel VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# No local paths in the binary (it is handed to testers): source paths that __FILE__ / std::source_location
# would embed are remapped, for CommonLibSSE-NG too, and the PDB is referenced by file name only.
# tools\Build.ps1 fails the build if a local path is still found in the DLL.
# CommonLibSSE-NG (alandtse/CommonLibVR, branch ng) is built from a checkout into this project's own build
# folder; nothing is written into that checkout. Give its path with -DCOMMONLIB_DIR=... or the environment
# variable COMMONLIB_DIR.
if(NOT COMMONLIB_DIR AND DEFINED ENV{COMMONLIB_DIR})
	file(TO_CMAKE_PATH "$ENV{COMMONLIB_DIR}" _la_commonlib_env)
	set(COMMONLIB_DIR "${_la_commonlib_env}" CACHE PATH "CommonLibSSE-NG checkout")
endif()
if(NOT COMMONLIB_DIR OR NOT EXISTS "${COMMONLIB_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "Set COMMONLIB_DIR to a CommonLibSSE-NG checkout (alandtse/CommonLibVR, branch ng).")
endif()
cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH _la_parent)
cmake_path(NATIVE_PATH _la_parent _la_parent_native)
cmake_path(NATIVE_PATH COMMONLIB_DIR _la_commonlib_native)
cmake_path(NATIVE_PATH CMAKE_BINARY_DIR _la_build_native)
add_compile_options("/d1trimfile:${_la_build_native}\\" "/d1trimfile:${_la_commonlib_native}\\" "/d1trimfile:${_la_parent_native}\\")
add_link_options("/PDBALTPATH:%_PDB%")

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${COMMONLIB_DIR} commonlib EXCLUDE_FROM_ALL)
if(NOT COMMAND add_commonlibsse_plugin)
	include(${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake)
endif()

# Built-in stage (README.md): 1 = count only, 2 = shadow index (compare, never use), 3 = replace.
# SKSE\Plugins\LoadAccel.ini can override it at run time.
set(LOADACCEL_STAGE 1 CACHE STRING "Built-in stage of the source-file-list target (0 to 3)")
set(LOADACCEL_B_STAGE 0 CACHE STRING "Built-in stage of the large-reference target (0 to 3)")

file(GLOB_RECURSE LOADACCEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_commonlibsse_plugin(${PROJECT_NAME}
	AUTHOR "LoadAccel"
	SOURCES ${LOADACCEL_SOURCES}
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor /utf-8 /permissive- /EHsc /W4)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
	LOADACCEL_STAGE=${LOADACCEL_STAGE} LOADACCEL_B_STAGE=${LOADACCEL_B_STAGE})
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
