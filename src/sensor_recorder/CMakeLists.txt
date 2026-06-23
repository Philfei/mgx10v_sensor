cmake_minimum_required(VERSION 3.16)
project(sensor_recorder CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3)

option(SENSOR_RECORDER_BUILD_APP "Build sensor_recorder application" ON)

get_filename_component(RECEIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)

add_library(sensor_recorder_core STATIC
  src/record_writer.cpp
  src/receiver_runtime.cpp
  src/recorder_config.cpp)
target_include_directories(sensor_recorder_core PUBLIC src)
target_link_libraries(sensor_recorder_core PUBLIC Threads::Threads)

if(SENSOR_RECORDER_BUILD_APP)
  set(SENSOR_RECORDER_THIRDPARTY_PREFIX
      "${RECEIVER_TEST_DIR}/thirdparty/install/aarch64-linux-gnu"
      CACHE PATH "sensors_code thirdparty install prefix for target ZMQ/protobuf")
  set(SENSOR_RECORDER_HOST_PROTOC
      "${RECEIVER_TEST_DIR}/thirdparty/install/host/bin/protoc"
      CACHE FILEPATH "host protoc used to generate C++ protobuf files")

  if(NOT EXISTS "${SENSOR_RECORDER_HOST_PROTOC}")
    message(FATAL_ERROR "host protoc not found: ${SENSOR_RECORDER_HOST_PROTOC}")
  endif()
  if(NOT EXISTS "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/include/zmq.hpp")
    message(FATAL_ERROR
        "ZMQ/protobuf thirdparty prefix is incomplete: ${SENSOR_RECORDER_THIRDPARTY_PREFIX}")
  endif()

  set(ZMQ_LIB "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libzmq.so")
  set(PROTOBUF_LIB "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libprotobuf.so")
  if(NOT EXISTS "${ZMQ_LIB}" OR NOT EXISTS "${PROTOBUF_LIB}")
    message(FATAL_ERROR
        "libzmq.so or libprotobuf.so is missing under ${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib")
  endif()

  set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sensor_data/proto")
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
    COMMAND "${SENSOR_RECORDER_HOST_PROTOC}"
            --cpp_out="${PROTO_GEN_DIR}"
            -I "${PROTO_DIR}"
            -I "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/include"
            ${PROTO_FILES}
    DEPENDS ${PROTO_FILES} "${SENSOR_RECORDER_HOST_PROTOC}"
    COMMENT "Generating sensor protobuf C++ sources")

  add_library(sensor_recorder_proto STATIC ${PROTO_SRCS})
  target_include_directories(sensor_recorder_proto PUBLIC
    "${PROTO_GEN_DIR}"
    "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/include")
  target_link_libraries(sensor_recorder_proto PUBLIC "${PROTOBUF_LIB}")

  add_executable(sensor_recorder
    src/main.cpp)
  target_include_directories(sensor_recorder PRIVATE
    src
    "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/include")
  target_link_libraries(sensor_recorder PRIVATE
    sensor_recorder_core
    sensor_recorder_proto
    "${ZMQ_LIB}"
    "${PROTOBUF_LIB}"
    Threads::Threads)
  target_link_options(sensor_recorder PRIVATE
    "-Wl,-rpath-link,${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib")
  set_target_properties(sensor_recorder PROPERTIES
    INSTALL_RPATH "$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH TRUE)

  set(SENSOR_RECORDER_RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
  set(SENSOR_RECORDER_RUNTIME_LIBS
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libzmq.so"
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libzmq.so.5"
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libzmq.so.5.2.5"
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libprotobuf.so"
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libprotobuf.so.32"
      "${SENSOR_RECORDER_THIRDPARTY_PREFIX}/lib/libprotobuf.so.3.21.12.0")
  set(SENSOR_RECORDER_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH
      "Directory used by the deploy target")

  add_custom_target(copy_runtime_deps ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_RECORDER_RUNTIME_LIB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SENSOR_RECORDER_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_RECORDER_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_RECORDER_DEPLOY_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_RECORDER_DEPLOY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SENSOR_RECORDER_RUNTIME_LIBS}
            "${SENSOR_RECORDER_RUNTIME_LIB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SENSOR_RECORDER_RUNTIME_LIBS}
            "${SENSOR_RECORDER_DEPLOY_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROTO_FILES}
            "${CMAKE_BINARY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROTO_FILES}
            "${SENSOR_RECORDER_DEPLOY_DIR}/proto"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
            "${CMAKE_BINARY_DIR}/config.yaml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
            "${SENSOR_RECORDER_DEPLOY_DIR}/config.yaml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_BINARY_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${SENSOR_RECORDER_DEPLOY_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:sensor_recorder>"
            "${SENSOR_RECORDER_DEPLOY_DIR}/sensor_recorder"
    DEPENDS sensor_recorder
    COMMENT "Copying sensor_recorder deployment bundle")

  install(TARGETS sensor_recorder RUNTIME DESTINATION .)
  install(DIRECTORY "${SENSOR_RECORDER_RUNTIME_LIB_DIR}/" DESTINATION lib)
  install(DIRECTORY "${CMAKE_BINARY_DIR}/proto" DESTINATION .)
  install(FILES config.yaml README.md DESTINATION .)

  add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
            --prefix ${SENSOR_RECORDER_DEPLOY_DIR}
    DEPENDS copy_runtime_deps
    COMMENT "Installing sensor_recorder deployment bundle to ${SENSOR_RECORDER_DEPLOY_DIR}")
endif()
