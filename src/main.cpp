#include "sfu/room.hpp"
#include "sfu/rtp_packet.hpp"

#include <iostream>
#include <memory>

int main() {
    sfu::Room room("demo-room");

    auto alice = std::make_shared<sfu::Participant>("alice");
    auto bob = std::make_shared<sfu::Participant>(
        "bob",
        [](const sfu::RtpPacket& packet) {
            std::cout << "bob received RTP seq=" << packet.sequenceNumber()
                      << " payload_type=" << static_cast<int>(packet.payloadType())
                      << " bytes=" << packet.size() << '\n';
        });

    room.addParticipant(alice);
    room.addParticipant(bob);

    auto packet = sfu::RtpPacket::create(
        96,
        1,
        48000,
        1234,
        std::vector<std::uint8_t>{0x11, 0x22, 0x33},
        true);

    const auto result = room.publishRtp("alice", packet);
    const auto metrics = room.metrics();

    std::cout << "SFU core ready. forwarded=" << result.recipients
              << " received_packets=" << metrics.totals.packets_received
              << " forwarded_packets=" << metrics.totals.packets_forwarded
              << '\n';

    return 0;
}
