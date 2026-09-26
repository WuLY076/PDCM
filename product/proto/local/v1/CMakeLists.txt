add_library(
  pdcm_local_protocol_proto
  STATIC
    pdcm_local.proto
)
add_library(PDCM::local_protocol_proto ALIAS pdcm_local_protocol_proto)
protobuf_generate(
  TARGET pdcm_local_protocol_proto
  LANGUAGE cpp
)
target_include_directories(
  pdcm_local_protocol_proto
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
)
target_link_libraries(
  pdcm_local_protocol_proto
  PUBLIC protobuf::libprotobuf
)
