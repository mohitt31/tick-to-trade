add_library(ttt STATIC
  header_check.cpp)
target_include_directories(ttt PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ttt
  PUBLIC itch ttt_options
  PRIVATE ttt_warnings)
