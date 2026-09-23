#include "ipc/frame.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pdcm::ipc {
namespace {

void writeU16(std::vector<std::uint8_t> *bytes, const std::size_t offset,
              const std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value & 0xFFU);
}

void writeU32(std::vector<std::uint8_t> *bytes, const std::size_t offset,
              const std::uint32_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value & 0xFFU);
}

void writeU64(std::vector<std::uint8_t> *bytes, const std::size_t offset,
              const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7U - index) * 8U;
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>((value >> shift) & 0xFFU);
  }
}

std::uint16_t readU16(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[0]) << 8U) |
      static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t readU32(const std::uint8_t *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t readU64(const std::uint8_t *bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
  }
  return value;
}

} // namespace

Status encodeFrame(const Frame &frame, const std::size_t max_frame_bytes,
                   std::vector<std::uint8_t> *output) {
  if (output == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "output buffer is null");
  }
  if (max_frame_bytes < kFrameHeaderBytes ||
      frame.payload.size() >
          std::numeric_limits<std::uint32_t>::max() - kFrameHeaderBytes) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "invalid frame size limit");
  }

  const std::size_t frame_size = kFrameHeaderBytes + frame.payload.size();
  if (frame_size > max_frame_bytes) {
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "frame exceeds configured maximum");
  }

  output->assign(frame_size, 0);
  writeU32(output, 0, static_cast<std::uint32_t>(frame_size));
  writeU16(output, 4, frame.protocol_major);
  writeU16(output, 6, frame.protocol_minor);
  writeU16(output, 8, static_cast<std::uint16_t>(frame.message_type));
  writeU16(output, 10, frame.flags);
  writeU64(output, 12, frame.request_id);
  std::copy(frame.payload.begin(), frame.payload.end(),
            output->begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes));
  return Status::success();
}

FrameDecoder::FrameDecoder(const std::size_t max_frame_bytes)
    : max_frame_bytes_(max_frame_bytes) {}

Status FrameDecoder::append(const std::uint8_t *data, const std::size_t size,
                            std::vector<Frame> *frames) {
  if (poisoned_) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "frame decoder is poisoned");
  }
  if (frames == nullptr || (data == nullptr && size != 0)) {
    return fail(PDCM_STATUS_INVALID_ARGUMENT, "invalid decoder argument");
  }
  if (size == 0) {
    return Status::success();
  }

  const std::size_t maximum_size = std::numeric_limits<std::size_t>::max();
  const std::size_t buffer_limit = max_frame_bytes_ > maximum_size / 2U
                                       ? maximum_size
                                       : max_frame_bytes_ * 2U;
  if (max_frame_bytes_ < kFrameHeaderBytes || size > max_frame_bytes_ ||
      buffer_.size() > max_frame_bytes_ ||
      size > buffer_limit - buffer_.size()) {
    return fail(PDCM_STATUS_RESOURCE_EXHAUSTED,
                "decoder buffer exceeds configured maximum");
  }

  buffer_.insert(buffer_.end(), data, data + size);

  std::size_t consumed = 0;
  while (buffer_.size() - consumed >= sizeof(std::uint32_t)) {
    const std::uint8_t *current = buffer_.data() + consumed;
    const std::uint32_t frame_size = readU32(current);
    if (frame_size < kFrameHeaderBytes) {
      return fail(PDCM_STATUS_INVALID_ARGUMENT,
                  "frame length is smaller than header");
    }
    if (frame_size > max_frame_bytes_) {
      return fail(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "frame length exceeds configured maximum");
    }
    if (buffer_.size() - consumed < frame_size) {
      break;
    }

    Frame frame;
    frame.protocol_major = readU16(current + 4);
    frame.protocol_minor = readU16(current + 6);
    frame.message_type = static_cast<MessageType>(readU16(current + 8));
    frame.flags = readU16(current + 10);
    frame.request_id = readU64(current + 12);
    frame.payload.assign(current + kFrameHeaderBytes, current + frame_size);
    frames->push_back(std::move(frame));
    consumed += frame_size;
  }

  if (consumed != 0) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
  }
  return Status::success();
}

void FrameDecoder::reset() noexcept {
  buffer_.clear();
  poisoned_ = false;
}

bool FrameDecoder::poisoned() const noexcept { return poisoned_; }

std::size_t FrameDecoder::bufferedBytes() const noexcept {
  return buffer_.size();
}

Status FrameDecoder::fail(const pdcm_status_t status, const char *message) {
  buffer_.clear();
  poisoned_ = true;
  return Status(status, message);
}

} // namespace pdcm::ipc
