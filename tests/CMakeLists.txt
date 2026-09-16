include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ttt_test(<name> SOURCES ... [LIBS ...])
function(ttt_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ttt ${T_LIBS} GTest::gtest_main ttt_test_warnings)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # PRE_TEST: discover when ctest runs, so a sanitizer abort is a failing test
  # rather than a broken build. The timeout turns a hang into a failure.
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300)
endfunction()

ttt_test(unit_mold SOURCES unit/mold_test.cpp)
ttt_test(unit_packet_stream SOURCES unit/packet_stream_test.cpp)
ttt_test(unit_pcap_audit SOURCES unit/pcap_audit_test.cpp)
