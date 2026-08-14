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

add_library(vektor_core
  src/config.cpp
  src/health_inspector.cpp
  src/reporter.cpp
  src/status.cpp
)
target_include_directories(vektor_core PUBLIC include)
target_link_libraries(vektor_core yaml-cpp)
ament_target_dependencies(vektor_core
  rclcpp
  rclcpp_action
  tf2
  tf2_ros
  lifecycle_msgs
)

add_executable(vektor src/main.cpp)
target_link_libraries(vektor vektor_core)
ament_target_dependencies(vektor rclcpp)

install(TARGETS vektor vektor_core
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/vektor
)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config/ DESTINATION share/${PROJECT_NAME}/config)
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
endif()

ament_export_include_directories(include)
ament_export_dependencies(rclcpp rclcpp_action tf2 tf2_ros lifecycle_msgs yaml-cpp)
ament_package()
