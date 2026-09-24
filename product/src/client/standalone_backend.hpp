#ifndef PDCM_CLIENT_STANDALONE_BACKEND_HPP_
#define PDCM_CLIENT_STANDALONE_BACKEND_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "client/backend.hpp"
#include "ipc/unix_socket.hpp"

namespace pdcm {

class StandaloneBackend final : public ClientBackend {
public:
  StandaloneBackend(std::string endpoint, std::chrono::milliseconds deadline,
                    std::size_t max_frame_bytes);
  ~StandaloneBackend() override;

  Status start() override;
  Status version(BackendVersion *version) const override;
  EntityListResult entities(EntityKind kind) const override;
  CapabilityQueryResult capabilities(EntityRef entity) const override;
  Status close() noexcept override;

private:
  Status exchangeLocked(ipc::MessageType request_type,
                        const std::string &request_payload,
                        ipc::MessageType expected_response_type,
                        ipc::Frame *response) const;
  std::string endpoint_;
  std::chrono::milliseconds deadline_;
  std::size_t max_frame_bytes_;

  mutable std::mutex mutex_;
  ipc::UniqueFd socket_;
  mutable std::uint64_t next_request_id_{2};
  BackendVersion version_;
  bool started_{false};
  bool closed_{false};
};

} // namespace pdcm

#endif // PDCM_CLIENT_STANDALONE_BACKEND_HPP_
