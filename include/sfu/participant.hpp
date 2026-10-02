#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sfu {

class RtpPacket;

using PacketSender = std::function<void(const RtpPacket&)>;

class Participant {
public:
    explicit Participant(std::string id);
    Participant(std::string id, PacketSender sender);

    const std::string& id() const noexcept;

    bool wantsMediaFrom(const std::string& publisher_id) const;
    void setReceivingEnabled(bool enabled) noexcept;
    bool receivingEnabled() const noexcept;

    void send(const RtpPacket& packet) const;

private:
    std::string id_;
    PacketSender sender_;
    bool receiving_enabled_ = true;
};

}  // namespace sfu
