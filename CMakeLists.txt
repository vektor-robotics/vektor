cmake_minimum_required(VERSION 3.16)
project(vektor)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(VEKTOR_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(VEKTOR_PROTO_FILE ${VEKTOR_PROTO_ROOT}/vektor/agent/v1/agent.proto)
set(VEKTOR_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VEKTOR_PROTO_SOURCE ${VEKTOR_GENERATED_DIR}/vektor/agent/v1/agent.pb.cc)
set(VEKTOR_PROTO_HEADER ${VEKTOR_GENERATED_DIR}/vektor/agent/v1/agent.pb.h)
set(VEKTOR_GRPC_SOURCE ${VEKTOR_GENERATED_DIR}/vektor/agent/v1/agent.grpc.pb.cc)
set(VEKTOR_GRPC_HEADER ${VEKTOR_GENERATED_DIR}/vektor/agent/v1/agent.grpc.pb.h)

add_custom_command(
  OUTPUT
    ${VEKTOR_PROTO_SOURCE}
    ${VEKTOR_PROTO_HEADER}
    ${VEKTOR_GRPC_SOURCE}
    ${VEKTOR_GRPC_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${VEKTOR_GENERATED_DIR}/vektor/agent/v1
  COMMAND protobuf::protoc
    --proto_path=${VEKTOR_PROTO_ROOT}
    --cpp_out=${VEKTOR_GENERATED_DIR}
    --grpc_out=${VEKTOR_GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${VEKTOR_PROTO_FILE}
  DEPENDS ${VEKTOR_PROTO_FILE}
  VERBATIM
)

add_library(vektor_proto
  ${VEKTOR_PROTO_SOURCE}
  ${VEKTOR_GRPC_SOURCE}
)
target_include_directories(vektor_proto PUBLIC
  $<BUILD_INTERFACE:${VEKTOR_GENERATED_DIR}>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(vektor_core
  src/config.cpp
  src/health_inspector.cpp
  src/reporter.cpp
  src/status.cpp
)
target_include_directories(vektor_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_core yaml-cpp)
ament_target_dependencies(vektor_core
  rclcpp
  rclcpp_action
  tf2
  tf2_ros
  lifecycle_msgs
)

add_library(vektor_deployment src/deployment.cpp src/runtime.cpp src/trust.cpp)
target_include_directories(vektor_deployment PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_deployment vektor_proto yaml-cpp Threads::Threads)

add_library(vektor_agent src/agent.cpp)
target_include_directories(vektor_agent PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_agent
  vektor_core
  vektor_deployment
  vektor_proto
  gRPC::grpc++
  Threads::Threads
)
ament_target_dependencies(vektor_agent rclcpp)

add_library(vektor_fleet src/fleet.cpp)
target_include_directories(vektor_fleet PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_fleet
  vektor_core
  vektor_proto
  gRPC::grpc++
  Threads::Threads
)

add_library(vektor_rollout src/rollout.cpp)
target_include_directories(vektor_rollout PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(vektor_rollout
  vektor_deployment
  vektor_fleet
  vektor_proto
  yaml-cpp
  gRPC::grpc++
  Threads::Threads
)

add_executable(vektor src/main.cpp)
target_link_libraries(vektor
  vektor_core vektor_agent vektor_fleet vektor_rollout)
ament_target_dependencies(vektor rclcpp)

install(TARGETS vektor vektor_core vektor_agent vektor_deployment vektor_fleet
  vektor_rollout vektor_proto
  EXPORT vektor_targets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/vektor
)
install(DIRECTORY include/ DESTINATION include)
install(FILES
  ${VEKTOR_PROTO_HEADER}
  ${VEKTOR_GRPC_HEADER}
  DESTINATION include/vektor/agent/v1
)
install(DIRECTORY config/ DESTINATION share/${PROJECT_NAME}/config)
install(DIRECTORY proto/ DESTINATION share/${PROJECT_NAME}/proto)
install(FILES README.md DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_config test/test_config.cpp)
  target_link_libraries(test_config vektor_core)
  ament_target_dependencies(test_config rclcpp)

  ament_add_gtest(test_health_inspector test/test_health_inspector.cpp)
  target_link_libraries(test_health_inspector vektor_core)
  ament_target_dependencies(test_health_inspector
    rclcpp
    std_msgs
    geometry_msgs
    lifecycle_msgs
    tf2_ros
  )

  ament_add_gtest(test_reporter test/test_reporter.cpp)
  target_link_libraries(test_reporter vektor_core)

  ament_add_gtest(test_status test/test_status.cpp)
  target_link_libraries(test_status vektor_core)

  ament_add_gtest(test_agent test/test_agent.cpp)
  target_link_libraries(test_agent vektor_agent vektor_proto gRPC::grpc++)

  ament_add_gtest(test_fleet test/test_fleet.cpp)
  target_link_libraries(test_fleet vektor_fleet vektor_agent vektor_proto
    gRPC::grpc++)

  ament_add_gtest(test_deployment test/test_deployment.cpp)
  target_link_libraries(test_deployment vektor_deployment vektor_proto)

  ament_add_gtest(test_trust test/test_trust.cpp)
  target_link_libraries(test_trust vektor_deployment)

  ament_add_gtest(test_rollout test/test_rollout.cpp)
  target_link_libraries(test_rollout vektor_rollout vektor_agent
    vektor_deployment vektor_proto gRPC::grpc++)
endif()

ament_export_include_directories(include)
ament_export_targets(vektor_targets HAS_LIBRARY_TARGET)
ament_export_dependencies(
  rclcpp
  rclcpp_action
  tf2
  tf2_ros
  lifecycle_msgs
  yaml-cpp
  Protobuf
  gRPC
  Threads
)
ament_package()
