#include "sfu/participant.hpp"

#include "sfu/rtp_packet.hpp"

#include <utility>

namespace sfu {

Participant::Participant(std::string id)
    : Participant(std::move(id), nullptr) {}

Participant::Participant(std::string id, PacketSender sender)
    : id_(std::move(id)),
      sender_(std::move(sender)) {}

const std::string& Participant::id() const noexcept {
    return id_;
}

bool Participant::wantsMediaFrom(const std::string& publisher_id) const {
    return receiving_enabled_ && publisher_id != id_;
}

void Participant::setReceivingEnabled(bool enabled) noexcept {
    receiving_enabled_ = enabled;
}

bool Participant::receivingEnabled() const noexcept {
    return receiving_enabled_;
}

void Participant::send(const RtpPacket& packet) const {
    if (sender_) {
        sender_(packet);
    }
}

}  // namespace sfu
