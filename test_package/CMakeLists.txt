cmake_minimum_required(VERSION 4.3)
project(soml-package-test LANGUAGES CXX)

find_package(soml 0.1 CONFIG REQUIRED)
add_executable(soml-package-test test.cpp)
target_link_libraries(soml-package-test PRIVATE soml::soml)
set_target_properties(soml-package-test PROPERTIES CXX_EXTENSIONS NO)

enable_testing()
add_test(NAME package-consumer COMMAND soml-package-test)

option(SOML_TEST_MODULE "Also test the installed named module" OFF)

if(SOML_TEST_MODULE)
	add_executable(soml-module-consumer ../tests/module.cpp ../tests/module-header.cpp)
	target_link_libraries(soml-module-consumer PRIVATE soml::module)
	set_target_properties(soml-module-consumer PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO CXX_SCAN_FOR_MODULES ON)
	add_test(NAME module-consumer COMMAND soml-module-consumer)
endif()
