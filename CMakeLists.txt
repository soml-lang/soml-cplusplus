cmake_minimum_required(VERSION 4.3)

project(soml VERSION 0.1.0 DESCRIPTION "A strict reader and writer for SOML, a config format for humans" LANGUAGES CXX)

include(GNUInstallDirs)

add_library(soml INTERFACE)
add_library(soml::soml ALIAS soml)
target_include_directories(soml INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# CMake 4.3 recognizes C++26 for all supported compilers.
target_compile_features(soml INTERFACE cxx_std_26)

if(PROJECT_IS_TOP_LEVEL)
	option(SOML_SANITIZE "Build the tests with AddressSanitizer, UndefinedBehaviorSanitizer, and the standard library's hardening" OFF)
	option(SOML_FUZZ "Build the libFuzzer harness, which needs a Clang with libFuzzer" OFF)

	set(CMAKE_CXX_EXTENSIONS OFF)
	set(SOML_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wundef -Werror)

	# Static reflection is only in GCC so far, behind a flag. GCC's -Wnull-dereference reports false positives inside libstdc++ at -O2, so it is on for Clang only.
	if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		add_compile_options(-freflection)
	else()
		list(APPEND SOML_WARNINGS -Wnull-dereference)
	endif()

	if(SOML_SANITIZE)
		add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
		add_link_options(-fsanitize=address,undefined)
		add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG _GLIBCXX_ASSERTIONS)
	endif()
endif()

option(SOML_MODULE "Build the named module for import soml (requires Ninja and module-capable Clang or GCC)" OFF)

if(SOML_MODULE)
	if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
		message(FATAL_ERROR "SOML_MODULE requires the Ninja or Ninja Multi-Config generator")
	endif()

	if(NOT ((CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 18) OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)))
		message(FATAL_ERROR "SOML_MODULE requires Clang 18+ with clang-scan-deps or GCC 14+; Apple Clang is not supported by CMake's module scanner")
	endif()

	add_library(soml-module STATIC)
	add_library(soml::module ALIAS soml-module)
	target_link_libraries(soml-module PUBLIC soml::soml)
	target_sources(soml-module PUBLIC FILE_SET CXX_MODULES BASE_DIRS modules FILES modules/soml.cppm)
	set_target_properties(soml-module PROPERTIES EXPORT_NAME module CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES ON)
endif()

option(SOML_INSTALL "Install the header and the CMake package" ${PROJECT_IS_TOP_LEVEL})

if(SOML_INSTALL)
	include(CMakePackageConfigHelpers)
	install(TARGETS soml EXPORT soml-targets)
	if(SOML_MODULE)
		install(TARGETS soml-module EXPORT soml-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_DATADIR}/soml/modules)
	endif()
	install(FILES include/soml.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
	install(FILES license DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/soml)
	install(EXPORT soml-targets NAMESPACE soml:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/soml FILE soml-config.cmake CXX_MODULES_DIRECTORY modules)
	write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/soml-config-version.cmake COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/soml-config-version.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/soml)
endif()

if(NOT PROJECT_IS_TOP_LEVEL)
	return()
endif()

enable_testing()

if(SOML_MODULE)
	add_executable(soml-module-test tests/module.cpp tests/module-header.cpp)
	target_link_libraries(soml-module-test PRIVATE soml::module)
	target_compile_options(soml-module-test PRIVATE ${SOML_WARNINGS})
	set_target_properties(soml-module-test PROPERTIES CXX_SCAN_FOR_MODULES ON)
	add_test(NAME module COMMAND soml-module-test)
	add_test(NAME module-build-options COMMAND ${CMAKE_COMMAND} "-DMODULE_COMPILE_OPTIONS=$<TARGET_PROPERTY:soml-module,COMPILE_OPTIONS>" "-DTEST_COMPILE_OPTIONS=$<TARGET_PROPERTY:soml-module-test,COMPILE_OPTIONS>" -DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID} -DREQUIRE_SANITIZERS=${SOML_SANITIZE} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check-module-options.cmake)
endif()

foreach(suite test date format edit polish numeric unicode allocation improvements hardening refinement value-performance parser-performance writer-performance streamlining value-refactor parser-refactor writer-refactor mapping-refactor)
	add_executable(soml-${suite} tests/${suite}.cpp)
	target_link_libraries(soml-${suite} PRIVATE soml::soml)
	target_compile_options(soml-${suite} PRIVATE ${SOML_WARNINGS})
	if(suite STREQUAL "test")
		add_test(NAME unit COMMAND soml-test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
	else()
		add_test(NAME ${suite} COMMAND soml-${suite} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
	endif()
endforeach()

add_executable(soml-readme tests/readme.cpp)
target_link_libraries(soml-readme PRIVATE soml::soml)
target_compile_options(soml-readme PRIVATE ${SOML_WARNINGS})
add_test(NAME readme COMMAND ${CMAKE_COMMAND} -DEXECUTABLE=$<TARGET_FILE:soml-readme> -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/readme.expected.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check-output.cmake)

add_executable(soml-tagged tests/tagged.cpp)
target_link_libraries(soml-tagged PRIVATE soml::soml)
target_compile_options(soml-tagged PRIVATE ${SOML_WARNINGS})

find_program(NODE node)

if(NODE)
	add_test(NAME conformance COMMAND ${NODE} --test tests/conformance.js WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
	set_tests_properties(conformance PROPERTIES ENVIRONMENT "SOML_TAGGED=$<TARGET_FILE:soml-tagged>")

	# Every conformance case again, in constant expressions: compiling the generated file is the test. The 100-level cases need more nested calls than the compilers allow by default.
	file(GLOB_RECURSE SOML_CONFORMANCE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/*)
	add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/constexpr-conformance.cpp
		COMMAND ${NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/generate-constexpr.js ${CMAKE_CURRENT_BINARY_DIR}/constexpr-conformance.cpp
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/generate-constexpr.js ${SOML_CONFORMANCE_FILES}
	)
	add_library(soml-constexpr-conformance OBJECT ${CMAKE_CURRENT_BINARY_DIR}/constexpr-conformance.cpp)
	target_link_libraries(soml-constexpr-conformance PRIVATE soml::soml)
	target_compile_options(soml-constexpr-conformance PRIVATE -fconstexpr-depth=4096)
else()
	message(STATUS "Node.js was not found, so the conformance tests are left out")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	add_test(NAME tooling COMMAND ${Python3_EXECUTABLE} tests/tooling.py WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

option(SOML_BENCH "Build the benchmark" OFF)
option(SOML_BENCH_COMPARE "Build the benchmark with nlohmann/json, simdjson, and toml++, which are downloaded at pinned releases and checked by hash" OFF)

if(SOML_BENCH OR SOML_BENCH_COMPARE)
	add_executable(soml-bench bench/bench.cpp)
	target_link_libraries(soml-bench PRIVATE soml::soml)
	target_compile_options(soml-bench PRIVATE -O2)
	set_source_files_properties(bench/bench.cpp PROPERTIES COMPILE_OPTIONS "${SOML_WARNINGS}")
	target_compile_definitions(soml-bench PRIVATE NDEBUG)
endif()

if(SOML_BENCH_COMPARE)
	set(SOML_BENCH_LIBRARIES ${CMAKE_CURRENT_BINARY_DIR}/bench-libraries)

	foreach(library
		"json.hpp|https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp|aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"
		"simdjson.h|https://raw.githubusercontent.com/simdjson/simdjson/v4.6.11/singleheader/simdjson.h|a5c467c33c7871262eb4c82775490430758939c1c516fbdaeb662f8255860a60"
		"simdjson.cpp|https://raw.githubusercontent.com/simdjson/simdjson/v4.6.11/singleheader/simdjson.cpp|66c818d1a6b3841febef7a8336ff297f0a52e250a436b598bcdecf010d626243"
		"toml.hpp|https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp|6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e"
	)
		string(REPLACE "|" ";" parts "${library}")
		list(GET parts 0 name)
		list(GET parts 1 url)
		list(GET parts 2 hash)

		file(DOWNLOAD ${url} ${SOML_BENCH_LIBRARIES}/${name} EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON TIMEOUT 120 INACTIVITY_TIMEOUT 30)
	endforeach()

	target_sources(soml-bench PRIVATE ${SOML_BENCH_LIBRARIES}/simdjson.cpp)
	target_include_directories(soml-bench SYSTEM PRIVATE ${SOML_BENCH_LIBRARIES})
	target_compile_definitions(soml-bench PRIVATE SOML_BENCH_COMPARE)
endif()

if(SOML_FUZZ)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
		message(FATAL_ERROR "SOML_FUZZ requires Clang with libFuzzer")
	endif()
	add_executable(soml-fuzz fuzz/fuzz.cpp)
	target_link_libraries(soml-fuzz PRIVATE soml::soml)
	target_compile_options(soml-fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
	target_link_options(soml-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
