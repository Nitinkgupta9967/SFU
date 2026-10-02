#include "sfu/participant.hpp"
#include "sfu/room.hpp"
#include "sfu/rtp_packet.hpp"
#include "sfu/transport.hpp"
#include "sfu/udp_transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint16_t kServerPort = 6400;
constexpr std::uint16_t kAlicePort = 6401;
constexpr std::uint16_t kBobPort = 6402;

void runServer(std::atomic<bool>& running) {
    sfu::UdpSocket socket;
    if (!socket.bind("127.0.0.1", kServerPort)) {
        std::cerr << "server bind failed: " << socket.lastError() << '\n';
        running = false;
        return;
    }

    sfu::Room room("loopback-demo");
    sfu::RoomRtpIngress ingress(room);
    std::unordered_map<std::string, sfu::UdpEndpoint> endpoints;

    std::cout << "server listening on 127.0.0.1:" << kServerPort << '\n';

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
                        if (!socket.sendTo(endpoint, bytes)) {
                            std::cerr << "server send to " << endpoint.id()
                                      << " failed: " << socket.lastError() << '\n';
                        }
                    }));

            room.addParticipant(participant);
            std::cout << "server joined " << participant_id
                      << " participants=" << room.participantCount() << '\n';
        }

        const auto result = ingress.receiveRawRtp(participant_id, received->bytes);
        const auto active_speaker = room.activeSpeakerId().value_or("none");
        std::cout << "server got RTP from " << participant_id
                  << " forwarded=" << result.forwarding.recipients
                  << " active=" << active_speaker << '\n';
    }
}

bool sendRtp(
    sfu::UdpSocket& socket,
    const std::string& label,
    std::uint16_t sequence_number,
    std::uint32_t ssrc) {
    const auto packet = sfu::RtpPacket::create(
        96,
        sequence_number,
        static_cast<std::uint32_t>(sequence_number * 3000),
        ssrc,
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(sequence_number & 0xffU)});

    const bool sent = socket.sendTo(
        sfu::UdpEndpoint{"127.0.0.1", kServerPort},
        packet.serialize());

    if (sent) {
        std::cout << label << " sent seq=" << sequence_number << '\n';
    }

    return sent;
}

int receiveForwarded(
    sfu::UdpSocket& socket,
    const std::string& label,
    int timeout_ms) {
    int received_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto received = socket.receiveWithTimeout(1500, 100);
        if (!received.has_value()) {
            continue;
        }

        const auto parsed = sfu::RtpPacket::parse(received->bytes);
        if (!parsed.has_value()) {
            continue;
        }

        ++received_count;
        std::cout << label << " received forwarded RTP seq="
                  << parsed->sequenceNumber()
                  << " ssrc=" << parsed->ssrc()
                  << " from " << received->sender.id() << '\n';
    }

    return received_count;
}

}  // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::atomic<bool> running{true};
    std::thread server([&running]() {
        runServer(running);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sfu::UdpSocket alice;
    sfu::UdpSocket bob;
    if (!alice.bind("127.0.0.1", kAlicePort)) {
        std::cerr << "alice bind failed: " << alice.lastError() << '\n';
        running = false;
        server.join();
        return 1;
    }
    if (!bob.bind("127.0.0.1", kBobPort)) {
        std::cerr << "bob bind failed: " << bob.lastError() << '\n';
        running = false;
        server.join();
        return 1;
    }

    sendRtp(alice, "alice", 1, 1001);
    sendRtp(bob, "bob", 1, 2002);
    sendRtp(bob, "bob", 2, 2002);

    const auto alice_received = receiveForwarded(alice, "alice", 1000);

    running = false;
    server.join();

    std::cout << "demo complete. alice_forwarded_packets=" << alice_received << '\n';

    return alice_received > 0 ? 0 : 1;
}
