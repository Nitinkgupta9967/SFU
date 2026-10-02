#include "sfu/rtp_packet.hpp"

#include <cstddef>
#include <utility>

namespace sfu {

namespace {

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

}  // namespace

std::optional<RtpPacket> RtpPacket::parse(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kFixedHeaderSize) {
        return std::nullopt;
    }

    const auto version = static_cast<std::uint8_t>(bytes[0] >> 6U);
    if (version != 2U) {
        return std::nullopt;
    }

    const bool has_extension = (bytes[0] & 0x10U) != 0;
    const auto csrc_count = static_cast<std::size_t>(bytes[0] & 0x0fU);
    std::size_t payload_offset = kFixedHeaderSize + (csrc_count * 4U);

    if (bytes.size() < payload_offset) {
        return std::nullopt;
    }

    if (has_extension) {
        if (bytes.size() < payload_offset + 4U) {
            return std::nullopt;
        }

        const auto extension_words = readU16(bytes, payload_offset + 2U);
        payload_offset += 4U + (static_cast<std::size_t>(extension_words) * 4U);
        if (bytes.size() < payload_offset) {
            return std::nullopt;
        }
    }

    RtpPacket packet;
    packet.marker_ = (bytes[1] & 0x80U) != 0;
    packet.payload_type_ = static_cast<std::uint8_t>(bytes[1] & 0x7fU);
    packet.sequence_number_ = readU16(bytes, 2);
    packet.timestamp_ = readU32(bytes, 4);
    packet.ssrc_ = readU32(bytes, 8);
    packet.payload_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset), bytes.end());
    packet.raw_ = bytes;

    return packet;
}

RtpPacket RtpPacket::create(
    std::uint8_t payload_type,
    std::uint16_t sequence_number,
    std::uint32_t timestamp,
    std::uint32_t ssrc,
    std::vector<std::uint8_t> payload,
    bool marker) {
    RtpPacket packet;
    packet.payload_type_ = static_cast<std::uint8_t>(payload_type & 0x7fU);
    packet.sequence_number_ = sequence_number;
    packet.timestamp_ = timestamp;
    packet.ssrc_ = ssrc;
    packet.marker_ = marker;
    packet.payload_ = std::move(payload);
    packet.raw_ = packet.serialize();

    return packet;
}

std::vector<std::uint8_t> RtpPacket::serialize() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kFixedHeaderSize + payload_.size());

    bytes.push_back(0x80U);
    bytes.push_back(static_cast<std::uint8_t>((marker_ ? 0x80U : 0x00U) | (payload_type_ & 0x7fU)));
    appendU16(bytes, sequence_number_);
    appendU32(bytes, timestamp_);
    appendU32(bytes, ssrc_);
    bytes.insert(bytes.end(), payload_.begin(), payload_.end());

    return bytes;
}

std::uint8_t RtpPacket::payloadType() const noexcept {
    return payload_type_;
}

std::uint16_t RtpPacket::sequenceNumber() const noexcept {
    return sequence_number_;
}

std::uint32_t RtpPacket::timestamp() const noexcept {
    return timestamp_;
}

std::uint32_t RtpPacket::ssrc() const noexcept {
    return ssrc_;
}

bool RtpPacket::marker() const noexcept {
    return marker_;
}

const std::vector<std::uint8_t>& RtpPacket::payload() const noexcept {
    return payload_;
}

const std::vector<std::uint8_t>& RtpPacket::raw() const noexcept {
    return raw_;
}

MediaKind RtpPacket::mediaKind() const noexcept {
    if (payload_type_ == 111U || payload_type_ == 0U || payload_type_ == 8U) {
        return MediaKind::Audio;
    }

    if (payload_type_ >= 96U && payload_type_ <= 127U) {
        return MediaKind::Video;
    }

    return MediaKind::Unknown;
}

std::size_t RtpPacket::size() const noexcept {
    return raw_.empty() ? kFixedHeaderSize + payload_.size() : raw_.size();
}

}  // namespace sfu
