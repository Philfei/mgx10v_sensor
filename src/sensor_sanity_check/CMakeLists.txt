cmake_minimum_required(VERSION 3.16)
project(sensor_sanity_check CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3)

get_filename_component(RECEIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(SENSOR_SANITY_RUNTIME_LIBS)
if(CMAKE_CROSSCOMPILING)
  set(SENSOR_SANITY_THIRDPARTY_PREFIX
      "${RECEIVER_TEST_DIR}/thirdparty/install/aarch64-linux-gnu"
      CACHE PATH "sensors_code thirdparty install prefix for target protobuf")
  set(PROTOBUF_LIB "${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib/libprotobuf.so")
  if(NOT EXISTS "${PROTOBUF_LIB}")
    message(FATAL_ERROR "libprotobuf.so is missing under ${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib")
  endif()
  set(SENSOR_SANITY_PROTOBUF_TARGET "${PROTOBUF_LIB}")
  set(SENSOR_SANITY_PROTOBUF_INCLUDE_DIRS
      "${SENSOR_SANITY_THIRDPARTY_PREFIX}/include")
  set(SENSOR_SANITY_RUNTIME_LIBS
      "${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib/libprotobuf.so"
      "${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib/libprotobuf.so.32"
      "${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib/libprotobuf.so.3.21.12.0")
else()
  find_package(Protobuf REQUIRED)
  set(SENSOR_SANITY_PROTOBUF_TARGET protobuf::libprotobuf)
  set(SENSOR_SANITY_PROTOBUF_INCLUDE_DIRS ${Protobuf_INCLUDE_DIRS})
endif()

add_library(sensor_sanity_core STATIC
  src/chunk_reader.cpp
  src/sanity_config.cpp
  src/timestamp_extractor.cpp)
target_include_directories(sensor_sanity_core PUBLIC
  src
  ${SENSOR_SANITY_PROTOBUF_INCLUDE_DIRS})
target_link_libraries(sensor_sanity_core PUBLIC ${SENSOR_SANITY_PROTOBUF_TARGET})

add_executable(sensor_sanity_check src/main.cpp)
target_link_libraries(sensor_sanity_check PRIVATE sensor_sanity_core)
if(CMAKE_CROSSCOMPILING)
  target_link_options(sensor_sanity_check PRIVATE
    "-Wl,-rpath-link,${SENSOR_SANITY_THIRDPARTY_PREFIX}/lib")
endif()
set_target_properties(sensor_sanity_check PROPERTIES
  INSTALL_RPATH "$ORIGIN/lib"
  BUILD_WITH_INSTALL_RPATH TRUE)

enable_testing()

add_executable(chunk_reader_test tests/chunk_reader_test.cpp)
target_link_libraries(chunk_reader_test PRIVATE sensor_sanity_core)
add_test(NAME chunk_reader_test COMMAND chunk_reader_test)

add_executable(sanity_config_test tests/sanity_config_test.cpp)
target_link_libraries(sanity_config_test PRIVATE sensor_sanity_core)
add_test(NAME sanity_config_test COMMAND sanity_config_test)

add_executable(timestamp_extractor_test tests/timestamp_extractor_test.cpp)
target_link_libraries(timestamp_extractor_test PRIVATE sensor_sanity_core)
add_test(NAME timestamp_extractor_test COMMAND timestamp_extractor_test)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sensor_data/proto")
set(PROTO_FILES
    "${PROTO_DIR}/Common.proto"
    "${PROTO_DIR}/RawImageMsg.proto"
    "${PROTO_DIR}/ImuMsg.proto"
    "${PROTO_DIR}/GnssMsg.proto")

set(SENSOR_SANITY_RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
set(SENSOR_SANITY_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH
    "Directory used by the deploy target")

add_custom_target(copy_runtime_deps ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_SANITY_RUNTIME_LIB_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${SENSOR_SANITY_DEPLOY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_SANITY_DEPLOY_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_SANITY_DEPLOY_DIR}/lib"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SENSOR_SANITY_DEPLOY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROTO_FILES}
          "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROTO_FILES}
          "${SENSOR_SANITY_DEPLOY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
          "${CMAKE_BINARY_DIR}/config.yaml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/config.yaml"
          "${SENSOR_SANITY_DEPLOY_DIR}/config.yaml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${CMAKE_BINARY_DIR}/README.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${SENSOR_SANITY_DEPLOY_DIR}/README.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:sensor_sanity_check>"
          "${SENSOR_SANITY_DEPLOY_DIR}/sensor_sanity_check"
  DEPENDS sensor_sanity_check
  COMMENT "Copying sensor_sanity_check deployment bundle")

if(SENSOR_SANITY_RUNTIME_LIBS)
  add_custom_command(TARGET copy_runtime_deps POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SENSOR_SANITY_RUNTIME_LIBS}
            "${SENSOR_SANITY_RUNTIME_LIB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SENSOR_SANITY_RUNTIME_LIBS}
            "${SENSOR_SANITY_DEPLOY_DIR}/lib"
    COMMENT "Copying target protobuf runtime libraries")
endif()

install(TARGETS sensor_sanity_check RUNTIME DESTINATION .)
install(DIRECTORY "${SENSOR_SANITY_RUNTIME_LIB_DIR}/" DESTINATION lib)
install(DIRECTORY "${CMAKE_BINARY_DIR}/proto" DESTINATION .)
install(FILES config.yaml README.md DESTINATION .)
