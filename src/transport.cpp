#include "sfu/transport.hpp"

#include "sfu/rtp_packet.hpp"

#include <utility>

namespace sfu {

PacketSender makeRtpPacketSender(RawPacketSender sender) {
    return [sender = std::move(sender)](const RtpPacket& packet) {
        if (!sender) {
            return;
        }

        const auto& raw = packet.raw();
        sender(raw.empty() ? packet.serialize() : raw);
    };
}

RoomRtpIngress::RoomRtpIngress(Room& room)
    : room_(room) {}

InboundRtpResult RoomRtpIngress::receiveRawRtp(
    const std::string& publisher_id,
    const std::vector<std::uint8_t>& bytes) {
    const auto packet = RtpPacket::parse(bytes);
    if (!packet.has_value()) {
        return InboundRtpResult{InboundRtpStatus::InvalidPacket, {}};
    }

    const auto forwarding = room_.publishRtp(publisher_id, *packet);
    const auto status = forwarding.dropped > 0
        ? InboundRtpStatus::UnknownPublisher
        : InboundRtpStatus::Forwarded;

    return InboundRtpResult{status, forwarding};
}

}  // namespace sfu
