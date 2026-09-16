add_library(ttt STATIC
  audit/sequence_audit.cpp
  live/feed_server.cpp
  live/receiver.cpp
  measure/histogram.cpp
  measure/manifest.cpp
  net/endpoint.cpp
  net/socket.cpp
  header_check.cpp
  pcap/pcap.cpp
  sim/chaos.cpp)
target_include_directories(ttt PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/generated)
add_dependencies(ttt ttt_version)
target_link_libraries(ttt
  PUBLIC itch ttt_options hdr_histogram_static
  PRIVATE ttt_warnings)

# Linux-only receive paths. io_uring needs liburing; without it the rest builds.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.3)
  add_library(ttt_rx STATIC rx/uring_path.cpp)
  target_link_libraries(ttt_rx PUBLIC ttt PkgConfig::URING PRIVATE ttt_warnings)
endif()
