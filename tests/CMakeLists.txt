include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ttt_test(<name> [SERIAL] SOURCES ... [LIBS ...])
function(ttt_test name)
  cmake_parse_arguments(T "SERIAL" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ttt ${T_LIBS} GTest::gtest_main ttt_test_warnings)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # PRE_TEST: discover when ctest runs, so a sanitizer abort is a failing test
  # rather than a broken build. The timeout turns a hang into a failure.
  set(serial FALSE)
  if(T_SERIAL)
    set(serial TRUE)
  endif()
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300 RUN_SERIAL ${serial})
endfunction()

ttt_test(unit_mold SOURCES unit/mold_test.cpp)
ttt_test(unit_packet_stream SOURCES unit/packet_stream_test.cpp)
ttt_test(unit_pcap_audit SOURCES unit/pcap_audit_test.cpp)
ttt_test(unit_line_handler SOURCES unit/line_handler_test.cpp)
ttt_test(unit_servers SOURCES unit/servers_test.cpp)
ttt_test(unit_measure SOURCES unit/measure_test.cpp)
ttt_test(property_chaos SOURCES property/chaos_test.cpp)

# Every planted bug must make the chaos run report a failing seed, and the same
# build with no bug switched on must pass, so a broken harness cannot look like
# a caught mutant.
if(TTT_MUTANTS)
  set(TTT_MUTANT_NAMES OverlapMisnumbered ApplyDuplicates IgnoreControlFrontier
      SlotSeqUnchecked SnapshotLabelOffByOne SnapshotReversedQueue)
  foreach(m IN LISTS TTT_MUTANT_NAMES)
    add_test(NAME mutant.${m}
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/mutants/expect_caught.sh $<TARGET_FILE:ttt_chaos> ${m})
    set_tests_properties(mutant.${m} PROPERTIES TIMEOUT 600)
  endforeach()
  add_test(NAME mutant.control COMMAND ttt_chaos --seeds 2000)
  set_tests_properties(mutant.control PROPERTIES TIMEOUT 600)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  ttt_test(linux_rx_paths SERIAL SOURCES linux/rx_paths_test.cpp LIBS ttt_rx)
endif()

# Real sockets over loopback. Serial: they bind ports and pace in real time.
ttt_test(e2e_loopback SERIAL SOURCES e2e/loopback_test.cpp)
