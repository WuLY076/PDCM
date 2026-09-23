if(NOT DEFINED PDCM_NM OR NOT DEFINED PDCM_LIBRARY)
  message(FATAL_ERROR "PDCM_NM and PDCM_LIBRARY are required")
endif()

execute_process(
  COMMAND "${PDCM_NM}" -D --defined-only "${PDCM_LIBRARY}"
  RESULT_VARIABLE nm_status
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error
)
if(NOT nm_status EQUAL 0)
  message(FATAL_ERROR "nm failed: ${nm_error}")
endif()

set(allowed_symbols
    pdcm_close
    pdcm_open
    pdcm_version_get)
set(observed_symbols)

string(REPLACE "\n" ";" symbol_lines "${nm_output}")
foreach(line IN LISTS symbol_lines)
  if(line STREQUAL "")
    continue()
  endif()

  string(REGEX MATCH "[^ ]+$" symbol "${line}")
  string(REGEX REPLACE "@@.*$" "" symbol "${symbol}")
  if(symbol STREQUAL "PDCM_0.1")
    continue()
  endif()

  list(FIND allowed_symbols "${symbol}" allowed_index)
  if(allowed_index EQUAL -1)
    message(FATAL_ERROR "unexpected public symbol: ${symbol}")
  endif()
  list(APPEND observed_symbols "${symbol}")
endforeach()

foreach(symbol IN LISTS allowed_symbols)
  list(FIND observed_symbols "${symbol}" observed_index)
  if(observed_index EQUAL -1)
    message(FATAL_ERROR "required public symbol is missing: ${symbol}")
  endif()
endforeach()
