#include "sfu/participant.hpp"
#include "sfu/room.hpp"
#include "sfu/transport.hpp"
#include "sfu/udp_transport.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

namespace {

std::uint16_t parsePort(const char* value, std::uint16_t fallback) {
    if (value == nullptr) {
        return fallback;
    }

    const auto parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0 || parsed > 65535) {
        return fallback;
    }

    return static_cast<std::uint16_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    const auto bind_host = argc > 1 ? std::string(argv[1]) : std::string("127.0.0.1");
    const auto bind_port = parsePort(argc > 2 ? argv[2] : nullptr, 5004);

    sfu::UdpSocket socket;
    if (!socket.bind(bind_host, bind_port)) {
        std::cerr << "failed to bind UDP socket: " << socket.lastError() << '\n';
        return 1;
    }

    sfu::Room room("udp-demo");
    sfu::RoomRtpIngress ingress(room);
    std::unordered_map<std::string, sfu::UdpEndpoint> endpoints;

    std::cout << "listening for RTP over UDP on " << bind_host << ':' << bind_port << '\n';
    std::cout << "send RTP datagrams from two local clients to watch forwarding happen\n";

    while (true) {
        auto received = socket.receive(1500);
        if (!received.has_value()) {
            std::cerr << "receive failed: " << socket.lastError() << '\n';
            continue;
        }

        const auto participant_id = received->sender.id();
        if (endpoints.find(participant_id) == endpoints.end()) {
            endpoints.emplace(participant_id, received->sender);

            auto participant = std::make_shared<sfu::Participant>(
                participant_id,
                sfu::makeRtpPacketSender(
                    [&socket, endpoint = received->sender](const std::vector<std::uint8_t>& bytes) {
                        if (!socket.sendTo(endpoint, bytes)) {
                            std::cerr << "send to " << endpoint.id()
                                      << " failed: " << socket.lastError() << '\n';
                        }
                    }));

            room.addParticipant(participant);
            std::cout << "joined " << participant_id
                      << " participants=" << room.participantCount() << '\n';
        }

        const auto result = ingress.receiveRawRtp(participant_id, received->bytes);
        if (result.status == sfu::InboundRtpStatus::InvalidPacket) {
            std::cout << "dropped invalid RTP from " << participant_id
                      << " bytes=" << received->bytes.size() << '\n';
            continue;
        }

        std::cout << "rtp from " << participant_id
                  << " bytes=" << received->bytes.size()
                  << " forwarded=" << result.forwarding.recipients
                  << " dropped=" << result.forwarding.dropped << '\n';
    }
}
