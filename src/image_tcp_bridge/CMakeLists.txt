cmake_minimum_required(VERSION 3.16)
project(image_tcp_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -O3)

get_filename_component(RECEIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Threads REQUIRED)

set(IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX
    "${RECEIVER_TEST_DIR}/thirdparty/install/aarch64-linux-gnu"
    CACHE PATH "sensors_code thirdparty install prefix for target ZMQ/protobuf")
set(IMAGE_TCP_BRIDGE_HOST_PROTOC
    "${RECEIVER_TEST_DIR}/thirdparty/install/host/bin/protoc"
    CACHE FILEPATH "host protoc used to generate C++ protobuf files")

if(NOT EXISTS "${IMAGE_TCP_BRIDGE_HOST_PROTOC}")
  message(FATAL_ERROR "host protoc not found: ${IMAGE_TCP_BRIDGE_HOST_PROTOC}")
endif()
if(NOT EXISTS "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/include/zmq.hpp")
  message(FATAL_ERROR
      "ZMQ/protobuf thirdparty prefix is incomplete: ${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}")
endif()

set(ZMQ_LIB "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libzmq.so")
set(PROTOBUF_LIB "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libprotobuf.so")
if(NOT EXISTS "${ZMQ_LIB}" OR NOT EXISTS "${PROTOBUF_LIB}")
  message(FATAL_ERROR "libzmq.so or libprotobuf.so is missing under ${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib")
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
  COMMAND "${IMAGE_TCP_BRIDGE_HOST_PROTOC}"
          --cpp_out="${PROTO_GEN_DIR}"
          -I "${PROTO_DIR}"
          -I "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/include"
          ${PROTO_FILES}
  DEPENDS ${PROTO_FILES} "${IMAGE_TCP_BRIDGE_HOST_PROTOC}"
  COMMENT "Generating image bridge protobuf C++ sources")

add_library(image_tcp_bridge_proto STATIC ${PROTO_SRCS})
target_include_directories(image_tcp_bridge_proto PUBLIC
  "${PROTO_GEN_DIR}"
  "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/include")
target_link_libraries(image_tcp_bridge_proto PUBLIC "${PROTOBUF_LIB}")

set(OPENCV_INCLUDE_CANDIDATES
    "${RECEIVER_TEST_DIR}/thirdparty/rockchip/aarch64-linux-gnu/include")
set(OPENCV_INCLUDE_DIR "")
foreach(_dir IN LISTS OPENCV_INCLUDE_CANDIDATES)
  if(EXISTS "${_dir}/opencv2/core.hpp")
    set(OPENCV_INCLUDE_DIR "${_dir}")
    break()
  endif()
endforeach()
if(NOT OPENCV_INCLUDE_DIR)
  message(FATAL_ERROR "OpenCV headers not found in ${RECEIVER_TEST_DIR}/thirdparty/rockchip/aarch64-linux-gnu/include")
endif()

set(OPENCV_LIB_DIR "${RECEIVER_TEST_DIR}/thirdparty/rockchip/aarch64-linux-gnu/lib")
set(OPENCV_LIBS
    "${OPENCV_LIB_DIR}/libopencv_imgcodecs.so"
    "${OPENCV_LIB_DIR}/libopencv_imgproc.so"
    "${OPENCV_LIB_DIR}/libopencv_core.so")
foreach(_lib IN LISTS OPENCV_LIBS)
  if(NOT EXISTS "${_lib}")
    message(FATAL_ERROR "OpenCV runtime library missing: ${_lib}")
  endif()
endforeach()

add_executable(image_tcp_bridge
  src/image_tcp_bridge.cpp)
target_include_directories(image_tcp_bridge PRIVATE
  "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/include"
  "${OPENCV_INCLUDE_DIR}")
target_link_libraries(image_tcp_bridge PRIVATE
  image_tcp_bridge_proto
  "${ZMQ_LIB}"
  "${PROTOBUF_LIB}"
  ${OPENCV_LIBS}
  Threads::Threads)
target_link_options(image_tcp_bridge PRIVATE
  "-Wl,-rpath-link,${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib"
  "-Wl,-rpath-link,${OPENCV_LIB_DIR}")
set_target_properties(image_tcp_bridge PROPERTIES
  INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/lib/rockchip"
  BUILD_WITH_INSTALL_RPATH TRUE)

set(IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
set(IMAGE_TCP_BRIDGE_RUNTIME_LIBS
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libzmq.so"
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libzmq.so.5"
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libzmq.so.5.2.5"
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libprotobuf.so"
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libprotobuf.so.32"
    "${IMAGE_TCP_BRIDGE_THIRDPARTY_PREFIX}/lib/libprotobuf.so.3.21.12.0")
file(GLOB ROCKCHIP_RUNTIME_LIBS CONFIGURE_DEPENDS "${OPENCV_LIB_DIR}/*.so*")

add_custom_target(copy_runtime_deps ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}/rockchip"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${IMAGE_TCP_BRIDGE_RUNTIME_LIBS}
          "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${ROCKCHIP_RUNTIME_LIBS}
          "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}/rockchip"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROTO_FILES}
          "${CMAKE_BINARY_DIR}/proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/recv_tcp_image.py"
          "${CMAKE_BINARY_DIR}/recv_tcp_image.py"
  COMMAND chmod +x "${CMAKE_BINARY_DIR}/recv_tcp_image.py"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/start_image_tcp_bridge.sh"
          "${CMAKE_BINARY_DIR}/start_image_tcp_bridge.sh"
  COMMAND chmod +x "${CMAKE_BINARY_DIR}/start_image_tcp_bridge.sh"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${CMAKE_BINARY_DIR}/README.md"
  DEPENDS image_tcp_bridge
  COMMENT "Copying image_tcp_bridge runtime files")

foreach(_opencv_name core imgproc imgcodecs photo)
  if(EXISTS "${OPENCV_LIB_DIR}/libopencv_${_opencv_name}.so")
    add_custom_command(TARGET copy_runtime_deps POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${OPENCV_LIB_DIR}/libopencv_${_opencv_name}.so"
              "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}/rockchip/libopencv_${_opencv_name}.so.413"
      COMMENT "Copying OpenCV SONAME libopencv_${_opencv_name}.so.413")
  endif()
endforeach()

install(TARGETS image_tcp_bridge RUNTIME DESTINATION .)
install(DIRECTORY "${IMAGE_TCP_BRIDGE_RUNTIME_LIB_DIR}/" DESTINATION lib)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/proto" DESTINATION .)
install(PROGRAMS recv_tcp_image.py DESTINATION .)
install(PROGRAMS start_image_tcp_bridge.sh DESTINATION .)
install(FILES README.md DESTINATION .)

set(IMAGE_TCP_BRIDGE_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy" CACHE PATH
    "Directory used by the deploy target")
add_custom_target(deploy
  COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
          --prefix ${IMAGE_TCP_BRIDGE_DEPLOY_DIR}
  DEPENDS copy_runtime_deps
  COMMENT "Installing image_tcp_bridge deployment bundle to ${IMAGE_TCP_BRIDGE_DEPLOY_DIR}")
