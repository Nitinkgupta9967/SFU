#include "sfu/participant.hpp"
#include "sfu/room.hpp"
#include "sfu/rtp_packet.hpp"
#include "sfu/transport.hpp"
#include "sfu/udp_transport.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void runServer(std::atomic<bool>& running) {
    sfu::UdpSocket socket;
    assert(socket.bind("127.0.0.1", 6300));

    sfu::Room room("udp-integration");
    sfu::RoomRtpIngress ingress(room);
    std::unordered_map<std::string, sfu::UdpEndpoint> endpoints;

    while (running.load()) {
        auto received = socket.receiveWithTimeout(1500, 100);
        if (!received.has_value()) {
            continue;
        }

        const auto participant_id = received->sender.id();
        if (endpoints.find(participant_id) == endpoints.end()) {
            endpoints.emplace(participant_id, received->sender);

            auto participant = std::make_shared<sfu::Participant>(
                participant_id,
                sfu::makeRtpPacketSender(
                    [&socket, endpoint = received->sender](const std::vector<std::uint8_t>& bytes) {
                        socket.sendTo(endpoint, bytes);
                    }));

            assert(room.addParticipant(participant));
        }

        ingress.receiveRawRtp(participant_id, received->bytes);
    }
}

void sendRtp(
    sfu::UdpSocket& socket,
    std::uint16_t sequence_number,
    std::uint32_t ssrc) {
    const auto packet = sfu::RtpPacket::create(
        96,
        sequence_number,
        static_cast<std::uint32_t>(sequence_number * 3000),
        ssrc,
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(sequence_number & 0xffU)});

    assert(socket.sendTo(sfu::UdpEndpoint{"127.0.0.1", 6300}, packet.serialize()));
}

}  // namespace

int main() {
    std::atomic<bool> running{true};
    std::thread server([&running]() {
        runServer(running);
    });

    sfu::UdpSocket first;
    sfu::UdpSocket second;
    assert(first.bind("127.0.0.1", 6301));
    assert(second.bind("127.0.0.1", 6302));

    sendRtp(first, 1, 1001);
    sendRtp(second, 2, 2002);
    sendRtp(second, 3, 2002);

    const auto received = first.receiveWithTimeout(1500, 2000);
    assert(received.has_value());
    assert(received->sender == (sfu::UdpEndpoint{"127.0.0.1", 6300}));

    const auto parsed = sfu::RtpPacket::parse(received->bytes);
    assert(parsed.has_value());
    assert(parsed->ssrc() == 2002);

    running = false;
    server.join();

    return 0;
}
