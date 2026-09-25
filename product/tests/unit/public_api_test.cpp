#include <cstdint>
#include <limits>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "pdcm/pdcm.h"

namespace {

TEST(PublicApiTest, ReturnsLocalVersionWithoutHandle) {
  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;

  ASSERT_EQ(pdcm_version_get(nullptr, &version), PDCM_STATUS_SUCCESS);
  EXPECT_STREQ(version.library_version, "0.1.0");
  EXPECT_EQ(version.public_abi_major, PDCM_ABI_VERSION_MAJOR);
  EXPECT_EQ(version.local_protocol_major, 1);
  EXPECT_EQ(version.mode, 0);
}

TEST(PublicApiTest, RejectsInvalidStructuresAndProtocolMajor) {
  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;
  version.header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_struct_header_t));
  EXPECT_EQ(pdcm_version_get(nullptr, &version), PDCM_STATUS_INVALID_ARGUMENT);

  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  options.required_protocol_major = 2;
  pdcm_handle_t *handle = nullptr;
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_PROTOCOL_INCOMPATIBLE);
  EXPECT_EQ(handle, nullptr);

  options = PDCM_OPEN_OPTIONS_INIT;
  options.required_protocol_minor = 1;
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_PROTOCOL_INCOMPATIBLE);
  EXPECT_EQ(handle, nullptr);

  options = PDCM_OPEN_OPTIONS_INIT;
  options.deadline_ns = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(handle, nullptr);

  options = PDCM_OPEN_OPTIONS_INIT;
  options.endpoint = "relative.sock";
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(handle, nullptr);
}

TEST(PublicApiTest, StandaloneFailsForMissingDaemon) {
  const std::string endpoint =
      "/tmp/pdcm-missing-" + std::to_string(::getpid()) + ".sock";
  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  options.endpoint = endpoint.c_str();

  pdcm_handle_t *handle = nullptr;
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(handle, nullptr);
}

TEST(PublicApiTest, EmbeddedOpenReturnsDiagnosableDegradedHandle) {
  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  options.mode = PDCM_MODE_EMBEDDED;
  options.target = PDCM_TARGET_FPGA;

  pdcm_handle_t *handle = nullptr;
  ASSERT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_SUCCESS);
  ASSERT_NE(handle, nullptr);

  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;
  ASSERT_EQ(pdcm_version_get(handle, &version), PDCM_STATUS_SUCCESS);
  EXPECT_EQ(version.mode, PDCM_MODE_EMBEDDED);
  EXPECT_EQ(version.target, PDCM_TARGET_FPGA);
  EXPECT_EQ(version.core_state, PDCM_CORE_STATE_DEGRADED);
  EXPECT_EQ(version.provider_state, PDCM_PROVIDER_STATE_UNAVAILABLE);
  EXPECT_EQ(version.provider_load_state, PDCM_PROVIDER_LOAD_NOT_ATTEMPTED);
  EXPECT_EQ(version.detail_status, PDCM_STATUS_UNAVAILABLE);
  EXPECT_GT(version.session_id, 0);
  std::size_t entity_count = 7;
  EXPECT_EQ(pdcm_entity_list(handle, nullptr, nullptr, &entity_count),
            PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(entity_count, 0);

  pdcm_entity_ref_t entity = PDCM_ENTITY_REF_INIT;
  entity.kind = PDCM_ENTITY_KIND_DEVICE;
  entity.generation = 1;
  pdcm_capability_set_t capabilities = PDCM_CAPABILITY_SET_INIT;
  EXPECT_EQ(pdcm_capability_query(handle, &entity, &capabilities),
            PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(capabilities.item_count, 0);
  pdcm_health_request_t health_request = PDCM_HEALTH_REQUEST_INIT;
  health_request.entity = entity;
  pdcm_health_result_t health = PDCM_HEALTH_RESULT_INIT;
  EXPECT_EQ(pdcm_health_query(handle, &health_request, &health),
            PDCM_STATUS_UNAVAILABLE);
  health_request.reserved = 1;
  EXPECT_EQ(pdcm_health_query(handle, &health_request, &health),
            PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(pdcm_health_query(handle, nullptr, &health),
            PDCM_STATUS_INVALID_ARGUMENT);

  EXPECT_EQ(pdcm_close(&handle), PDCM_STATUS_SUCCESS);
  EXPECT_EQ(handle, nullptr);
  EXPECT_EQ(pdcm_close(&handle), PDCM_STATUS_INVALID_ARGUMENT);
}

TEST(PublicApiTest, AutoFallbackRequiresExplicitFlagAndTarget) {
  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  options.mode = PDCM_MODE_AUTO;
  options.target = PDCM_TARGET_FPGA;
  const std::string endpoint =
      "/tmp/pdcm-auto-missing-" + std::to_string(::getpid()) + ".sock";
  options.endpoint = endpoint.c_str();

  pdcm_handle_t *handle = nullptr;
  EXPECT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_UNAVAILABLE);

  options.flags = PDCM_OPEN_ALLOW_EMBEDDED_FALLBACK;
  ASSERT_EQ(pdcm_open(&options, &handle), PDCM_STATUS_SUCCESS);
  EXPECT_NE(handle, nullptr);
  EXPECT_EQ(pdcm_close(&handle), PDCM_STATUS_SUCCESS);
}

} // namespace
