#include "sfu/rtp_packet.hpp"
#include "sfu/udp_transport.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

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

int parseInt(const char* value, int fallback) {
    if (value == nullptr) {
        return fallback;
    }

    const auto parsed = std::strtol(value, nullptr, 10);
    return parsed <= 0 ? fallback : static_cast<int>(parsed);
}

void printUsage(const char* executable) {
    std::cout << "usage: " << executable
              << " <local-port> [server-host] [server-port] [packet-count] [linger-ms]\n"
              << "example: " << executable << " 6001 127.0.0.1 5004 20 10000\n";
}

void receiveForwardedPackets(
    sfu::UdpSocket& socket,
    std::atomic<bool>& running,
    std::atomic<int>& forwarded_packets) {
    while (running.load()) {
        const auto received = socket.receiveWithTimeout(1500, 250);
        if (!received.has_value()) {
            if (!socket.lastError().empty()) {
                std::cout << "receive skipped: " << socket.lastError() << '\n';
            }
            continue;
        }

        const auto parsed = sfu::RtpPacket::parse(received->bytes);
        if (parsed.has_value()) {
            ++forwarded_packets;
            std::cout << "received forwarded RTP from " << received->sender.id()
                      << " seq=" << parsed->sequenceNumber()
                      << " ssrc=" << parsed->ssrc() << '\n';
        } else {
            std::cout << "received non-RTP datagram from " << received->sender.id()
                      << " bytes=" << received->bytes.size() << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const auto local_port = parsePort(argv[1], 0);
    const auto server_host = argc > 2 ? std::string(argv[2]) : std::string("127.0.0.1");
    const auto server_port = parsePort(argc > 3 ? argv[3] : nullptr, 5004);
    const auto packet_count = parseInt(argc > 4 ? argv[4] : nullptr, 10);
    const auto linger_ms = parseInt(argc > 5 ? argv[5] : nullptr, 10000);

    if (local_port == 0) {
        std::cerr << "local port must be between 1 and 65535\n";
        return 1;
    }

    sfu::UdpSocket socket;
    if (!socket.bind("127.0.0.1", local_port)) {
        std::cerr << "failed to bind local UDP socket: " << socket.lastError() << '\n';
        return 1;
    }

    const sfu::UdpEndpoint server{server_host, server_port};
    const auto ssrc = static_cast<std::uint32_t>(100000 + local_port);

    std::cout << "sending " << packet_count << " RTP packets from 127.0.0.1:"
              << local_port << " to " << server.id() << '\n';

    std::atomic<bool> receiver_running{true};
    std::atomic<int> forwarded_packets{0};
    std::thread receiver(
        [&socket, &receiver_running, &forwarded_packets]() {
            receiveForwardedPackets(socket, receiver_running, forwarded_packets);
        });

    for (int index = 0; index < packet_count; ++index) {
        const auto sequence = static_cast<std::uint16_t>(index + 1);
        const auto packet = sfu::RtpPacket::create(
            96,
            sequence,
            static_cast<std::uint32_t>(sequence * 3000),
            ssrc,
            std::vector<std::uint8_t>{
                static_cast<std::uint8_t>(local_port & 0xffU),
                static_cast<std::uint8_t>(sequence & 0xffU)});

        if (!socket.sendTo(server, packet.serialize())) {
            std::cerr << "send failed: " << socket.lastError() << '\n';
            return 1;
        }

        std::cout << "sent seq=" << sequence << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "listening for forwarded RTP for " << linger_ms << " ms\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(linger_ms));
    receiver_running = false;
    receiver.join();

    std::cout << "done. forwarded_packets_received=" << forwarded_packets.load() << '\n';
    return 0;
}
