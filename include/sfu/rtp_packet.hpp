#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sfu {

enum class MediaKind {
    Audio,
    Video,
    Unknown
};

class RtpPacket {
public:
    static constexpr std::size_t kFixedHeaderSize = 12;

    static std::optional<RtpPacket> parse(const std::vector<std::uint8_t>& bytes);
    static RtpPacket create(
        std::uint8_t payload_type,
        std::uint16_t sequence_number,
        std::uint32_t timestamp,
        std::uint32_t ssrc,
        std::vector<std::uint8_t> payload,
        bool marker = false);

    std::vector<std::uint8_t> serialize() const;

    std::uint8_t payloadType() const noexcept;
    std::uint16_t sequenceNumber() const noexcept;
    std::uint32_t timestamp() const noexcept;
    std::uint32_t ssrc() const noexcept;
    bool marker() const noexcept;

    const std::vector<std::uint8_t>& payload() const noexcept;
    const std::vector<std::uint8_t>& raw() const noexcept;

    MediaKind mediaKind() const noexcept;
    std::size_t size() const noexcept;

private:
    RtpPacket() = default;

    std::uint8_t payload_type_ = 0;
    std::uint16_t sequence_number_ = 0;
    std::uint32_t timestamp_ = 0;
    std::uint32_t ssrc_ = 0;
    bool marker_ = false;
    std::vector<std::uint8_t> payload_;
    std::vector<std::uint8_t> raw_;
};

}  // namespace sfu
