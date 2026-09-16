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

  # AF_XDP. The XDP program is compiled with clang for the BPF target and
  # embedded through a libbpf skeleton, so the binary carries its own program.
  pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)
  find_program(BPF_CLANG clang REQUIRED)
  find_program(BPFTOOL bpftool REQUIRED)
  set(bpf_obj ${PROJECT_BINARY_DIR}/generated_bpf/ttt_xdp.bpf.o)
  set(bpf_skel ${PROJECT_BINARY_DIR}/generated_bpf/ttt_xdp.skel.h)
  file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/generated_bpf)
  add_custom_command(
    OUTPUT ${bpf_obj}
    COMMAND ${BPF_CLANG} -target bpf -O2 -g -Wall -Werror
            -I/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_redirect.bpf.c -o ${bpf_obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_redirect.bpf.c
    COMMENT "Compiling the XDP program")
  add_custom_command(
    OUTPUT ${bpf_skel}
    COMMAND ${BPFTOOL} gen skeleton ${bpf_obj} name ttt_xdp > ${bpf_skel}
    DEPENDS ${bpf_obj}
    COMMENT "Generating the XDP skeleton")
  add_custom_target(ttt_xdp_skel DEPENDS ${bpf_skel})

  add_library(ttt_xdp STATIC xdp/xsk.cpp)
  add_dependencies(ttt_xdp ttt_xdp_skel)
  # Generated code, not held to this project's warning set.
  target_include_directories(ttt_xdp SYSTEM PRIVATE ${PROJECT_BINARY_DIR}/generated_bpf)
  target_link_libraries(ttt_xdp PUBLIC ttt_rx PkgConfig::LIBBPF PRIVATE ttt_warnings)
endif()
