if(NOT DEFINED VEKTOR_EXECUTABLE OR NOT DEFINED RUN_CONFIG OR
   NOT DEFINED STATE_DIRECTORY)
  message(FATAL_ERROR "capture CLI test arguments are required")
endif()

get_filename_component(state_parent "${STATE_DIRECTORY}" DIRECTORY)
set(artifact_directory "${state_parent}/artifacts/navigation-baseline-001")
set(export_directory "${state_parent}/capture-cli-export")
file(REMOVE_RECURSE "${STATE_DIRECTORY}" "${artifact_directory}"
     "${export_directory}")

execute_process(
  COMMAND "${VEKTOR_EXECUTABLE}" capture start --config "${RUN_CONFIG}"
          --state-dir "${STATE_DIRECTORY}" --format json
  RESULT_VARIABLE start_result
  OUTPUT_VARIABLE start_output
  ERROR_VARIABLE start_error)
if(NOT start_result EQUAL 0 OR
   NOT start_output MATCHES "\"status\":\"active\"")
  message(FATAL_ERROR "capture start failed: ${start_error}${start_output}")
endif()

execute_process(
  COMMAND "${VEKTOR_EXECUTABLE}" capture show
          --run-id navigation-baseline-001 --state-dir "${STATE_DIRECTORY}"
          --format json
  RESULT_VARIABLE show_result
  OUTPUT_VARIABLE show_output
  ERROR_VARIABLE show_error)
if(NOT show_result EQUAL 0 OR
   NOT show_output MATCHES "\"policy_sha256\":\"sha256:[0-9a-f]+\"")
  message(FATAL_ERROR "capture show failed: ${show_error}${show_output}")
endif()

execute_process(
  COMMAND "${VEKTOR_EXECUTABLE}" capture stop
          --run-id navigation-baseline-001 --outcome passed
          --annotation "no localization drift" --metric goal_error_m=0.25
          --state-dir "${STATE_DIRECTORY}"
          --format json
  RESULT_VARIABLE stop_result
  OUTPUT_VARIABLE stop_output
  ERROR_VARIABLE stop_error)
if(NOT stop_result EQUAL 0 OR
   NOT stop_output MATCHES "\"status\":\"completed\"" OR
   NOT stop_output MATCHES "\"outcome\":\"passed\"" OR
   NOT stop_output MATCHES "\"goal_error_m\":0.25" OR
   NOT stop_output MATCHES "\"kind\":\"rosbag2\"")
  message(FATAL_ERROR "capture stop failed: ${stop_error}${stop_output}")
endif()

file(READ "${STATE_DIRECTORY}/navigation-baseline-001.yaml"
     candidate_manifest)
string(REPLACE "navigation-baseline-001" "navigation-candidate-001"
       candidate_manifest "${candidate_manifest}")
string(REPLACE "outcome: passed" "outcome: failed"
       candidate_manifest "${candidate_manifest}")
string(REPLACE "goal_error_m: 0.25" "goal_error_m: 0.5"
       candidate_manifest "${candidate_manifest}")
file(WRITE "${STATE_DIRECTORY}/navigation-candidate-001.yaml"
     "${candidate_manifest}")

execute_process(
  COMMAND "${VEKTOR_EXECUTABLE}" compare
          --baseline navigation-baseline-001
          --candidate navigation-candidate-001
          --state-dir "${STATE_DIRECTORY}" --format json
  RESULT_VARIABLE compare_result
  OUTPUT_VARIABLE compare_output
  ERROR_VARIABLE compare_error)
if(NOT compare_result EQUAL 0 OR
   NOT compare_output MATCHES "\"different\":true" OR
   NOT compare_output MATCHES "\"changed\":true" OR
   NOT compare_output MATCHES "\"name\":\"goal_error_m\"" OR
   NOT compare_output MATCHES "\"delta\":0.25")
  message(FATAL_ERROR "compare failed: ${compare_error}${compare_output}")
endif()

execute_process(
  COMMAND "${VEKTOR_EXECUTABLE}" capture export
          --run-id navigation-baseline-001 --state-dir "${STATE_DIRECTORY}"
          --output "${export_directory}" --format json
  RESULT_VARIABLE export_result
  OUTPUT_VARIABLE export_output
  ERROR_VARIABLE export_error)
if(NOT export_result EQUAL 0 OR
   NOT EXISTS "${export_directory}/manifest.yaml" OR
   NOT EXISTS "${export_directory}/manifest.json")
  message(FATAL_ERROR
          "capture export failed: ${export_error}${export_output}")
endif()

file(REMOVE_RECURSE "${STATE_DIRECTORY}" "${artifact_directory}"
     "${export_directory}")
