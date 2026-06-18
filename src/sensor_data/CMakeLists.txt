cmake_minimum_required(VERSION 3.16)
project(sensor_data_publishers CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3)

get_filename_component(RECEIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)

set(SENSOR_DATA_THIRDPARTY_PREFIX
    "${RECEIVER_TEST_DIR}/thirdparty/install/aarch64-linux-gnu"
    CACHE PATH "sensors_code thirdparty install prefix for target ZMQ/protobuf")
set(SENSOR_DATA_HOST_PROTOC
    "${RECEIVER_TEST_DIR}/thirdparty/install/host/bin/protoc"
    CACHE FILEPATH "host protoc used to generate C++ protobuf files")

if(NOT EXISTS "${SENSOR_DATA_HOST_PROTOC}")
  message(FATAL_ERROR "host protoc not found: ${SENSOR_DATA_HOST_PROTOC}")
endif()
if(NOT EXISTS "${SENSOR_DATA_THIRDPARTY_PREFIX}/include/zmq.hpp")
  message(FATAL_ERROR
      "ZMQ/protobuf thirdparty prefix is incomplete: ${SENSOR_DATA_THIRDPARTY_PREFIX}")
endif()

set(ZMQ_INCLUDE_DIR "${SENSOR_DATA_THIRDPARTY_PREFIX}/include")
set(ZMQ_LIB "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libzmq.so")
set(PROTOBUF_LIB "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libprotobuf.so")
if(NOT EXISTS "${ZMQ_LIB}" OR NOT EXISTS "${PROTOBUF_LIB}")
  message(FATAL_ERROR "libzmq.so or libprotobuf.so is missing under ${SENSOR_DATA_THIRDPARTY_PREFIX}/lib")
endif()

find_path(RGA_INCLUDE_DIR
  NAMES im2d.h
  PATH_SUFFIXES rga)
find_library(RGA_LIB
  NAMES rga)
if(NOT RGA_INCLUDE_DIR OR NOT RGA_LIB)
  message(FATAL_ERROR "Rockchip RGA development files not found in the toolchain sysroot")
endif()

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")
set(PROTO_FILES
    "${PROTO_DIR}/Common.proto"
    "${PROTO_DIR}/RawImageMsg.proto"
    "${PROTO_DIR}/ImuMsg.proto"
    "${PROTO_DIR}/GnssMsg.proto")
set(PROTO_SRCS)
set(PROTO_HDRS)
foreach(_proto IN LISTS PROTO_FILES)
  get_filename_component(_name "${_proto}" NAME_WE)
  list(APPEND PROTO_SRCS "${PROTO_GEN_DIR}/${_name}.pb.cc")
  list(APPEND PROTO_HDRS "${PROTO_GEN_DIR}/${_name}.pb.h")
endforeach()

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
  COMMAND "${SENSOR_DATA_HOST_PROTOC}"
          --cpp_out="${PROTO_GEN_DIR}"
          -I "${PROTO_DIR}"
          -I "${SENSOR_DATA_THIRDPARTY_PREFIX}/include"
          ${PROTO_FILES}
  DEPENDS ${PROTO_FILES} "${SENSOR_DATA_HOST_PROTOC}"
  COMMENT "Generating sensor protobuf C++ sources")

add_library(sensor_proto STATIC ${PROTO_SRCS})
target_include_directories(sensor_proto PUBLIC
  "${PROTO_GEN_DIR}"
  "${SENSOR_DATA_THIRDPARTY_PREFIX}/include")
target_link_libraries(sensor_proto PUBLIC "${PROTOBUF_LIB}")

add_library(sensor_common STATIC
  src/serial_port.cpp
  src/zmq_publisher.cpp)
target_include_directories(sensor_common PUBLIC
  include
  "${PROTO_GEN_DIR}"
  "${SENSOR_DATA_THIRDPARTY_PREFIX}/include")
target_link_libraries(sensor_common PUBLIC
  sensor_proto
  "${ZMQ_LIB}"
  "${PROTOBUF_LIB}"
  Threads::Threads)

find_path(RKAIQ_UAPI2_INCLUDE_DIR
  NAMES rk_aiq_user_api2_sysctl.h
  PATH_SUFFIXES rkaiq/uAPI2)
if(NOT RKAIQ_UAPI2_INCLUDE_DIR)
  message(FATAL_ERROR "RKAIQ headers not found in the toolchain sysroot")
endif()
get_filename_component(RKAIQ_INCLUDE_ROOT
                       "${RKAIQ_UAPI2_INCLUDE_DIR}/.." ABSOLUTE)

set(VENDOR_INC)
foreach(_dir
    "${RKAIQ_INCLUDE_ROOT}"
    "${RKAIQ_INCLUDE_ROOT}/uAPI2"
    "${RKAIQ_INCLUDE_ROOT}/common"
    "${RKAIQ_INCLUDE_ROOT}/xcore"
    "${RKAIQ_INCLUDE_ROOT}/algos"
    "${RKAIQ_INCLUDE_ROOT}/iq_parser"
    "${RKAIQ_INCLUDE_ROOT}/iq_parser_v2"
    "${RKAIQ_INCLUDE_ROOT}/smartIr")
  if(EXISTS "${_dir}")
    list(APPEND VENDOR_INC "${_dir}")
  endif()
endforeach()
foreach(_dir
    "${RECEIVER_TEST_DIR}/thirdparty/rockchip/aarch64-linux-gnu/include/rockit")
  if(EXISTS "${_dir}")
    list(APPEND VENDOR_INC "${_dir}")
  endif()
endforeach()

set(ROCKCHIP_LIB_CANDIDATE_DIRS
    "${RECEIVER_TEST_DIR}/thirdparty/rockchip/aarch64-linux-gnu/lib")
set(LOCAL_ROCKCHIP_LIB_DIR "")
foreach(_dir IN LISTS ROCKCHIP_LIB_CANDIDATE_DIRS)
  if(EXISTS "${_dir}/librockit.so"
     AND EXISTS "${_dir}/librkaiq.so"
     AND EXISTS "${_dir}/librockchip_mpp.so")
    set(LOCAL_ROCKCHIP_LIB_DIR "${_dir}")
    break()
  endif()
endforeach()

if(NOT LOCAL_ROCKCHIP_LIB_DIR)
  message(FATAL_ERROR
      "Rockchip camera runtime libraries not found. Expected librockit.so, "
      "librkaiq.so, and librockchip_mpp.so in one known vendor directory.")
endif()

file(GLOB ROCKCHIP_DEPLOY_LIBS CONFIGURE_DEPENDS
     "${LOCAL_ROCKCHIP_LIB_DIR}/*.so*")
set(ROCKCHIP_LIBS
    "${LOCAL_ROCKCHIP_LIB_DIR}/librockit.so"
    "${LOCAL_ROCKCHIP_LIB_DIR}/librkaiq.so"
    "${LOCAL_ROCKCHIP_LIB_DIR}/librockchip_mpp.so")
if(EXISTS "${LOCAL_ROCKCHIP_LIB_DIR}/libasound.so.2")
  list(APPEND ROCKCHIP_LIBS "${LOCAL_ROCKCHIP_LIB_DIR}/libasound.so.2")
endif()
if(EXISTS "${LOCAL_ROCKCHIP_LIB_DIR}/libdrm.so.2")
  list(APPEND ROCKCHIP_LIBS "${LOCAL_ROCKCHIP_LIB_DIR}/libdrm.so.2")
endif()

add_executable(cam_sensor_data
  src/cam_sensor_data.cpp)
target_include_directories(cam_sensor_data PRIVATE
  include
  ${VENDOR_INC}
  "${RGA_INCLUDE_DIR}")
target_compile_definitions(cam_sensor_data PRIVATE
  RKPLATFORM=1
  UAPI2
  RKAIQ)
target_link_libraries(cam_sensor_data PRIVATE
  sensor_common
  "${RGA_LIB}"
  ${ROCKCHIP_LIBS})
target_link_options(cam_sensor_data PRIVATE
  "-Wl,-rpath-link,${LOCAL_ROCKCHIP_LIB_DIR}"
  "-Wl,-rpath-link,${SENSOR_DATA_THIRDPARTY_PREFIX}/lib")

add_executable(imu_sensor_data
  src/imu_sensor_data.cpp)
target_link_libraries(imu_sensor_data PRIVATE sensor_common)

add_executable(gnss_sensor_data
  src/gnss_sensor_data.cpp)
target_link_libraries(gnss_sensor_data PRIVATE sensor_common)

add_executable(timestamp_conversion_test
  tests/timestamp_conversion_test.cpp)
target_include_directories(timestamp_conversion_test PRIVATE include)
target_link_libraries(timestamp_conversion_test PRIVATE sensor_common)

add_executable(gnss_fill_test
  tests/gnss_fill_test.cpp)
target_include_directories(gnss_fill_test PRIVATE include)
target_link_libraries(gnss_fill_test PRIVATE sensor_common)

add_executable(bounded_frame_queue_test
  tests/bounded_frame_queue_test.cpp)
target_include_directories(bounded_frame_queue_test PRIVATE include)

add_executable(frame_timestamp_tracker_test
  tests/frame_timestamp_tracker_test.cpp)
target_include_directories(frame_timestamp_tracker_test PRIVATE include)

add_executable(exposure_timestamp_test
  tests/exposure_timestamp_test.cpp)
target_include_directories(exposure_timestamp_test PRIVATE include)

add_executable(camera_timestamp_assignment_test
  tests/camera_timestamp_assignment_test.cpp)
target_include_directories(camera_timestamp_assignment_test PRIVATE include)

foreach(_target cam_sensor_data imu_sensor_data gnss_sensor_data timestamp_conversion_test gnss_fill_test bounded_frame_queue_test frame_timestamp_tracker_test exposure_timestamp_test camera_timestamp_assignment_test)
  set_target_properties(${_target} PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/lib/rockchip"
    BUILD_WITH_INSTALL_RPATH TRUE)
endforeach()

set(SENSOR_DATA_RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
set(SENSOR_DATA_RUNTIME_LIBS
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libzmq.so"
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libzmq.so.5"
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libzmq.so.5.2.5"
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libprotobuf.so"
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libprotobuf.so.32"
    "${SENSOR_DATA_THIRDPARTY_PREFIX}/lib/libprotobuf.so.3.21.12.0")

get_filename_component(RGA_LIB_DIR "${RGA_LIB}" DIRECTORY)
file(GLOB RGA_RUNTIME_LIBS CONFIGURE_DEPENDS "${RGA_LIB_DIR}/librga.so*")
list(APPEND SENSOR_DATA_RUNTIME_LIBS ${RGA_RUNTIME_LIBS})

add_custom_target(copy_runtime_deps ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_DATA_RUNTIME_LIB_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${SENSOR_DATA_RUNTIME_LIBS}
          "${SENSOR_DATA_RUNTIME_LIB_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROTO_FILES}
          "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${CMAKE_BINARY_DIR}/README.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/CAMERA_RESOLUTION.md"
          "${CMAKE_BINARY_DIR}/CAMERA_RESOLUTION.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/DEPLOYMENT.md"
          "${CMAKE_BINARY_DIR}/DEPLOYMENT.md"
  COMMAND ${CMAKE_COMMAND} -E rm -rf
          "${CMAKE_BINARY_DIR}/thirdparty"
  DEPENDS cam_sensor_data imu_sensor_data gnss_sensor_data
  COMMENT "Copying ZMQ/protobuf runtime libraries")

set(SENSOR_DATA_CONTROL_SCRIPTS
    start_vendor_pwm.sh
    stop_vendor_pwm.sh
    start_sensor_data.sh
    stop_sensor_data.sh)
foreach(_script IN LISTS SENSOR_DATA_CONTROL_SCRIPTS)
  add_custom_command(TARGET copy_runtime_deps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${_script}"
            "${CMAKE_BINARY_DIR}/${_script}"
    COMMAND chmod +x "${CMAKE_BINARY_DIR}/${_script}"
    COMMENT "Copying ${_script}")
endforeach()

if(ROCKCHIP_DEPLOY_LIBS)
  add_custom_command(TARGET copy_runtime_deps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_DATA_RUNTIME_LIB_DIR}/rockchip"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ROCKCHIP_DEPLOY_LIBS}
            "${SENSOR_DATA_RUNTIME_LIB_DIR}/rockchip"
    COMMENT "Copying Rockchip camera runtime libraries")
endif()

install(TARGETS cam_sensor_data imu_sensor_data gnss_sensor_data
        RUNTIME DESTINATION .)
install(DIRECTORY "${SENSOR_DATA_RUNTIME_LIB_DIR}/" DESTINATION lib)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/proto" DESTINATION .)
install(FILES README.md DESTINATION . OPTIONAL)
install(FILES CAMERA_RESOLUTION.md DESTINATION . OPTIONAL)
install(FILES DEPLOYMENT.md DESTINATION . OPTIONAL)
install(FILES ${SENSOR_DATA_CONTROL_SCRIPTS}
        DESTINATION .
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

set(SENSOR_DATA_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH
    "Directory used by the deploy target")
add_custom_target(deploy ALL
  COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
          --prefix ${SENSOR_DATA_DEPLOY_DIR}
  DEPENDS copy_runtime_deps
  COMMENT "Installing sensor_data deployment bundle to ${SENSOR_DATA_DEPLOY_DIR}")
