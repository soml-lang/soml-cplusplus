execute_process(COMMAND "${EXECUTABLE}" RESULT_VARIABLE result OUTPUT_VARIABLE actual ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Example failed (${result}): ${errors}")
endif()

file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
	message(FATAL_ERROR "Example output differs.\nExpected:\n${expected}\nActual:\n${actual}")
endif()
