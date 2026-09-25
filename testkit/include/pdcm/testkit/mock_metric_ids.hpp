#ifndef PDCM_TESTKIT_MOCK_METRIC_IDS_HPP_
#define PDCM_TESTKIT_MOCK_METRIC_IDS_HPP_

#include <cstdint>

namespace pdcm::testkit {

inline constexpr std::uint32_t kTestGaugeMetricId = 0xF0000001U;
inline constexpr std::uint32_t kTestCounterMetricId = 0xF0000002U;
inline constexpr std::uint32_t kTestDerivedRateMetricId = 0xF0000003U;
inline constexpr std::uint32_t kTestPeriodicFailureMetricId = 0xF0000004U;
inline constexpr std::uint32_t kTestHeartbeatEvidenceId = 0xF00000F0U;

} // namespace pdcm::testkit

#endif // PDCM_TESTKIT_MOCK_METRIC_IDS_HPP_
