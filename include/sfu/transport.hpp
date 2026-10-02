#pragma once

#include "sfu/participant.hpp"
#include "sfu/room.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sfu {

using RawPacketSender = std::function<void(const std::vector<std::uint8_t>&)>;

enum class InboundRtpStatus {
    Forwarded,
    InvalidPacket,
    UnknownPublisher
};

struct InboundRtpResult {
    InboundRtpStatus status = InboundRtpStatus::InvalidPacket;
    ForwardingResult forwarding;
};

PacketSender makeRtpPacketSender(RawPacketSender sender);

class RoomRtpIngress {
public:
    explicit RoomRtpIngress(Room& room);

    InboundRtpResult receiveRawRtp(
        const std::string& publisher_id,
        const std::vector<std::uint8_t>& bytes);

private:
    Room& room_;
};

}  // namespace sfu
