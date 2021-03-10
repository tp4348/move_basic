cmake_minimum_required(VERSION 2.8.3)
project(move_smooth)

find_package(catkin REQUIRED COMPONENTS
  roscpp
  tf2_geometry_msgs
  tf2_ros
  tf2
  geometry_msgs
  std_msgs
  actionlib
  actionlib_msgs
  sensor_msgs
  visualization_msgs
  move_base_msgs
  nav_core
  dynamic_reconfigure
  message_generation
)

add_service_files(
  FILES
  Stop.srv
)

# Generate dynamic reconfigure params
generate_dynamic_reconfigure_options(
  cfg/MoveSmooth.cfg
)

generate_messages(
  DEPENDENCIES
  std_msgs
)

catkin_package(INCLUDE_DIRS DEPENDS)

###########
## Build ##
###########

add_definitions(-std=c++11 -Wall -Wextra)

include_directories(${catkin_INCLUDE_DIRS} include)

add_executable(move_smooth src/collision_checker.cpp src/obstacle_points.cpp src/move_smooth.cpp)
add_dependencies(move_smooth ${${PROJECT_NAME}_EXPORTED_TARGETS}
                 ${catkin_EXPORTED_TARGETS})
target_link_libraries(move_smooth ${catkin_LIBRARIES})

#############
## Install ##
#############

## Mark executables and/or libraries for installation
install(TARGETS move_smooth
   ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
   LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
   RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)
