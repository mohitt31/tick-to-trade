add_library(ttt STATIC
  audit/sequence_audit.cpp
  live/feed_server.cpp
  live/receiver.cpp
  net/endpoint.cpp
  net/socket.cpp
  header_check.cpp
  pcap/pcap.cpp
  sim/chaos.cpp)
target_include_directories(ttt PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ttt
  PUBLIC itch ttt_options
  PRIVATE ttt_warnings)
