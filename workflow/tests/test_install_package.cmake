file(REMOVE_RECURSE "${INSTALL_PREFIX}" "${CONSUMER_BUILD}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_stdout
  ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "workflow package install failed (${install_result})\n${install_stdout}\n${install_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${CONSUMER_BUILD}"
          "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "workflow consumer configure failed (${configure_result})\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "workflow consumer build failed (${build_result})\n${build_stdout}\n${build_stderr}")
endif()

execute_process(
  COMMAND "${CONSUMER_BUILD}/workflow_package_consumer"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "workflow consumer run failed (${run_result})\n${run_stdout}\n${run_stderr}")
endif()
