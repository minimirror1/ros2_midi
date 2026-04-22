cmake_minimum_required(VERSION 3.16)
project(xtouch_midi CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RTMIDI REQUIRED IMPORTED_TARGET rtmidi)

add_executable(xtouch_node src/xtouch_node.cpp)
ament_target_dependencies(xtouch_node rclcpp std_msgs)
target_link_libraries(xtouch_node PkgConfig::RTMIDI)

install(TARGETS xtouch_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()
