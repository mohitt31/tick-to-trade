add_executable(ttt_replay replay.cpp)
target_link_libraries(ttt_replay PRIVATE ttt ZLIB::ZLIB ttt_warnings)

add_executable(ttt_pcap_audit pcap_audit.cpp)
target_link_libraries(ttt_pcap_audit PRIVATE ttt ttt_warnings)
