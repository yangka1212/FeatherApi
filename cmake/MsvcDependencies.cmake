# CMake's AUTO decoding can corrupt /showIncludes on localized MSVC when the
# parent process has no console. Ninja needs the compiler's exact prefix bytes.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/FeatherIncludes")
  file(MAKE_DIRECTORY "${probe_dir}")
  file(WRITE "${probe_dir}/probe.h" "\n")
  file(WRITE "${probe_dir}/probe.cpp" "#include \"probe.h\"\n")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /c probe.cpp
    WORKING_DIRECTORY "${probe_dir}"
    OUTPUT_VARIABLE probe_output ERROR_VARIABLE probe_error
    RESULT_VARIABLE probe_result ENCODING NONE)
  if(probe_result EQUAL 0 AND probe_output MATCHES "(^|\n)([^:\n]+:[^:\n]+:[ \t]+)[A-Za-z]:")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
  endif()
endif()
