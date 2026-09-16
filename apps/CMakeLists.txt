add_executable(ttt_replay replay.cpp)
target_link_libraries(ttt_replay PRIVATE ttt ZLIB::ZLIB ttt_warnings)

add_executable(ttt_pcap_audit pcap_audit.cpp)
target_link_libraries(ttt_pcap_audit PRIVATE ttt ttt_warnings)

add_executable(ttt_chaos chaos.cpp)
target_link_libraries(ttt_chaos PRIVATE ttt ttt_warnings)

add_executable(ttt_feedd feedd.cpp)
target_link_libraries(ttt_feedd PRIVATE ttt ZLIB::ZLIB ttt_warnings)

add_executable(ttt_recv recv.cpp)
target_link_libraries(ttt_recv PRIVATE ttt ttt_warnings)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ttt_rxbench rxbench.cpp)
  target_link_libraries(ttt_rxbench PRIVATE ttt_rx ttt_warnings)
endif()
