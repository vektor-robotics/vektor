if(NOT DEFINED VEKTOR_EXECUTABLE OR NOT DEFINED TESTBED_SCRIPT OR
   NOT DEFINED EVIDENCE_DIRECTORY)
  message(FATAL_ERROR "validation testbed arguments are required")
endif()

file(REMOVE_RECURSE "${EVIDENCE_DIRECTORY}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
          "VEKTOR_EXECUTABLE=${VEKTOR_EXECUTABLE}"
          bash "${TESTBED_SCRIPT}" "${EVIDENCE_DIRECTORY}"
  RESULT_VARIABLE testbed_result
  OUTPUT_VARIABLE testbed_output
  ERROR_VARIABLE testbed_error)
if(NOT testbed_result EQUAL 0)
  message(FATAL_ERROR
          "validation testbed failed: ${testbed_error}${testbed_output}")
endif()
if(NOT EXISTS "${EVIDENCE_DIRECTORY}/summary.json")
  message(FATAL_ERROR "validation testbed did not retain summary.json")
endif()
file(READ "${EVIDENCE_DIRECTORY}/summary.json" summary)
if(NOT summary MATCHES "\"replayed_messages\": 15" OR
   NOT summary MATCHES "\"top_candidate\": \"fast-publisher\"" OR
   NOT summary MATCHES "\"automatic_deployment\": false" OR
   NOT summary MATCHES "simulated ROS 2 only; no physical correlation")
  message(FATAL_ERROR "validation testbed summary is incomplete: ${summary}")
endif()
