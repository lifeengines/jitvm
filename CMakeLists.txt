cmake_minimum_required(VERSION 3.16)

project(jitvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
	add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W3> $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc>)
else()
	add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

include(FetchContent)

FetchContent_Declare(
	zycore
	GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
	GIT_TAG v1.5.2
	GIT_SHALLOW TRUE
)
FetchContent_GetProperties(zycore)
if(NOT zycore_POPULATED)
	FetchContent_Populate(zycore)
endif()

set(ZYAN_ZYCORE_PATH "${zycore_SOURCE_DIR}" CACHE PATH "" FORCE)
set(ZYDIS_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_MAN OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_DECODER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_ENCODER ON CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_FORMATTER OFF CACHE BOOL "" FORCE)
set(ZYDIS_FEATURE_AVX512 ON CACHE BOOL "" FORCE)
# v4.1.1 refuses the encoder unless KNC is also enabled outside minimal mode.
set(ZYDIS_FEATURE_KNC ON CACHE BOOL "" FORCE)
set(ZYDIS_MINIMAL_MODE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	zydis
	GIT_REPOSITORY https://github.com/zyantific/zydis.git
	GIT_TAG v4.1.1
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zydis)
if(NOT TARGET Zydis::Zydis)
	add_library(Zydis::Zydis ALIAS Zydis)
endif()
if(MSVC)
	set_property(TARGET Zydis PROPERTY INTERFACE_COMPILE_OPTIONS "")
	set_property(TARGET Zycore PROPERTY INTERFACE_COMPILE_OPTIONS "")
endif()

set(JITVM_HOST_X86 0)
set(JITVM_HOST_X64 0)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
	set(JITVM_HOST_X64 1)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x64|X64)$")
	set(JITVM_HOST_X64 1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86|X86|Win32)$" OR (CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM"))
	set(JITVM_HOST_X86 1)
endif()

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(JITVM_HOST_X64 1)
	set(JITVM_HOST_X86 0)
elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
	set(JITVM_HOST_X86 1)
	set(JITVM_HOST_X64 0)
endif()

set(JITVM_SOURCES
	src/platform/lock.cpp
	src/platform/memory.cpp
	src/platform/thread_ctx.cpp
	src/vm_api.cpp
)

if(JITVM_HOST_X86 OR JITVM_HOST_X64)
	list(APPEND JITVM_SOURCES
		src/arch/x86/codec.cpp
		src/arch/x86/emit.cpp
		src/arch/x86/engine.cpp
		src/arch/x86/filter.cpp
	)
	if(MSVC)
		enable_language(ASM_MASM)
		if(JITVM_HOST_X64)
			list(APPEND JITVM_SOURCES src/arch/x86/runtime/runtime_x64.asm)
		else()
			list(APPEND JITVM_SOURCES src/arch/x86/runtime/runtime_x86.asm)
		endif()
	else()
		enable_language(ASM)
		if(JITVM_HOST_X64)
			list(APPEND JITVM_SOURCES src/arch/x86/runtime/runtime_x64.S)
		else()
			list(APPEND JITVM_SOURCES src/arch/x86/runtime/runtime_x86.S)
		endif()
	endif()
else()
	list(APPEND JITVM_SOURCES src/arch/stub.cpp)
endif()

add_library(jitvm STATIC ${JITVM_SOURCES})
add_library(jitvm::jitvm ALIAS jitvm)
if(MSVC)
	target_compile_options(jitvm PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/wd4201>)
endif()

target_include_directories(jitvm
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(jitvm PUBLIC Zydis::Zydis)

if(JITVM_HOST_X64)
	target_compile_definitions(jitvm PUBLIC JITVM_HOST_X64=1)
elseif(JITVM_HOST_X86)
	target_compile_definitions(jitvm PUBLIC JITVM_HOST_X86=1)
else()
	target_compile_definitions(jitvm PUBLIC JITVM_HOST_UNSUPPORTED=1)
endif()

if(WIN32)
	target_compile_definitions(jitvm PRIVATE JITVM_OS_WINDOWS=1)
elseif(APPLE)
	target_compile_definitions(jitvm PRIVATE JITVM_OS_MACOS=1)
else()
	target_compile_definitions(jitvm PRIVATE JITVM_OS_LINUX=1)
endif()

if(NOT MSVC)
	target_link_libraries(jitvm PUBLIC pthread)
endif()

enable_testing()
add_subdirectory(tests)

if(JITVM_HOST_X86 OR JITVM_HOST_X64)
	add_executable(jitvm_guest_demo examples/guest_demo.cpp)
	if(MSVC)
		if(JITVM_HOST_X64)
			target_sources(jitvm_guest_demo PRIVATE examples/enter_x64.asm)
		else()
			target_sources(jitvm_guest_demo PRIVATE examples/enter_x86.asm)
		endif()
	else()
		if(JITVM_HOST_X64)
			target_sources(jitvm_guest_demo PRIVATE examples/enter_x64.S)
		else()
			target_sources(jitvm_guest_demo PRIVATE examples/enter_x86.S)
		endif()
	endif()
	target_link_libraries(jitvm_guest_demo PRIVATE jitvm)
	if(MSVC)
		target_compile_options(jitvm_guest_demo PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
		target_link_options(jitvm_guest_demo PRIVATE /INCREMENTAL:NO)
	endif()
endif()
