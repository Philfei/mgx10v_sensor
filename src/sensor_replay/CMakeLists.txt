cmake_minimum_required(VERSION 3.16)
project(sensor_replay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3)

option(SENSOR_REPLAY_BUILD_APP "Build sensor_replay application" ON)

get_filename_component(RECEIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)

set(SENSOR_REPLAY_RUNTIME_LIBS)
if(CMAKE_CROSSCOMPILING)
  set(SENSOR_REPLAY_THIRDPARTY_PREFIX
      "${RECEIVER_TEST_DIR}/thirdparty/install/aarch64-linux-gnu"
      CACHE PATH "sensors_code thirdparty install prefix for target ZMQ/protobuf")
  set(PROTOBUF_LIB "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libprotobuf.so")
  set(ZMQ_LIB "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libzmq.so")
  if(NOT EXISTS "${PROTOBUF_LIB}" OR NOT EXISTS "${ZMQ_LIB}")
    message(FATAL_ERROR "libprotobuf.so or libzmq.so is missing under ${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib")
  endif()
  set(SENSOR_REPLAY_PROTOBUF_TARGET "${PROTOBUF_LIB}")
  set(SENSOR_REPLAY_PROTOBUF_INCLUDE_DIRS
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/include")
  set(SENSOR_REPLAY_ZMQ_TARGET "${ZMQ_LIB}")
  set(SENSOR_REPLAY_ZMQ_INCLUDE_DIRS
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/include")
  set(SENSOR_REPLAY_RUNTIME_LIBS
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libzmq.so"
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libzmq.so.5"
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libzmq.so.5.2.5"
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libprotobuf.so"
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libprotobuf.so.32"
      "${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib/libprotobuf.so.3.21.12.0")
else()
  find_package(Protobuf REQUIRED)
  find_path(ZMQ_INCLUDE_DIR zmq.hpp)
  find_library(ZMQ_LIB zmq)
  set(SENSOR_REPLAY_PROTOBUF_TARGET protobuf::libprotobuf)
  set(SENSOR_REPLAY_PROTOBUF_INCLUDE_DIRS ${Protobuf_INCLUDE_DIRS})
  set(SENSOR_REPLAY_ZMQ_TARGET "${ZMQ_LIB}")
  set(SENSOR_REPLAY_ZMQ_INCLUDE_DIRS "${ZMQ_INCLUDE_DIR}")
endif()

add_library(sensor_replay_core STATIC
  src/replay_config.cpp
  src/replay_index.cpp
  src/replay_timing.cpp)
target_include_directories(sensor_replay_core PUBLIC
  src
  ${SENSOR_REPLAY_PROTOBUF_INCLUDE_DIRS})
target_link_libraries(sensor_replay_core PUBLIC
  ${SENSOR_REPLAY_PROTOBUF_TARGET}
  Threads::Threads)

if(SENSOR_REPLAY_BUILD_APP)
  if(NOT SENSOR_REPLAY_ZMQ_TARGET OR NOT SENSOR_REPLAY_ZMQ_INCLUDE_DIRS)
    message(FATAL_ERROR "ZMQ development files not found; configure with -DSENSOR_REPLAY_BUILD_APP=OFF for tests only")
  endif()
  add_executable(sensor_replay src/main.cpp)
  target_include_directories(sensor_replay PRIVATE
    src
    ${SENSOR_REPLAY_ZMQ_INCLUDE_DIRS})
  target_link_libraries(sensor_replay PRIVATE
    sensor_replay_core
    ${SENSOR_REPLAY_ZMQ_TARGET}
    Threads::Threads)
  if(CMAKE_CROSSCOMPILING)
    target_link_options(sensor_replay PRIVATE
      "-Wl,-rpath-link,${SENSOR_REPLAY_THIRDPARTY_PREFIX}/lib")
  endif()
  set_target_properties(sensor_replay PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH TRUE)
endif()

if(SENSOR_REPLAY_BUILD_APP)
  set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sensor_data/proto")
  set(PROTO_FILES
      "${PROTO_DIR}/Common.proto"
      "${PROTO_DIR}/RawImageMsg.proto"
      "${PROTO_DIR}/ImuMsg.proto"
      "${PROTO_DIR}/GnssMsg.proto")

  set(SENSOR_REPLAY_RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
  set(SENSOR_REPLAY_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH
      "Directory used by the deploy target")

  add_custom_target(copy_runtime_deps ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_REPLAY_RUNTIME_LIB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SENSOR_REPLAY_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_REPLAY_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_REPLAY_DEPLOY_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_REPLAY_DEPLOY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROTO_FILES}
            "${CMAKE_BINARY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROTO_FILES}
            "${SENSOR_REPLAY_DEPLOY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
            "${CMAKE_BINARY_DIR}/config.yaml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
            "${SENSOR_REPLAY_DEPLOY_DIR}/config.yaml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_BINARY_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${SENSOR_REPLAY_DEPLOY_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:sensor_replay>"
            "${SENSOR_REPLAY_DEPLOY_DIR}/sensor_replay"
    DEPENDS sensor_replay
    COMMENT "Copying sensor_replay deployment bundle")

  if(SENSOR_REPLAY_RUNTIME_LIBS)
    add_custom_command(TARGET copy_runtime_deps POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${SENSOR_REPLAY_RUNTIME_LIBS}
              "${SENSOR_REPLAY_RUNTIME_LIB_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${SENSOR_REPLAY_RUNTIME_LIBS}
              "${SENSOR_REPLAY_DEPLOY_DIR}/lib"
      COMMENT "Copying target ZMQ/protobuf runtime libraries")
  endif()

  install(TARGETS sensor_replay RUNTIME DESTINATION .)
  install(DIRECTORY "${SENSOR_REPLAY_RUNTIME_LIB_DIR}/" DESTINATION lib)
  install(DIRECTORY "${CMAKE_BINARY_DIR}/proto" DESTINATION .)
  install(FILES config.yaml README.md DESTINATION .)
endif()
