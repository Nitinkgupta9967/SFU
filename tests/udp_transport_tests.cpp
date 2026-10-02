#include "sfu/rtp_packet.hpp"
#include "sfu/udp_transport.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    sfu::UdpSocket first;
    sfu::UdpSocket second;

    assert(first.bind("127.0.0.1", 6201));
    assert(second.bind("127.0.0.1", 6202));

    const auto packet = sfu::RtpPacket::create(96, 44, 9000, 123456, {1, 2, 3});
    assert(first.sendTo(sfu::UdpEndpoint{"127.0.0.1", 6202}, packet.serialize()));

    const auto received = second.receiveWithTimeout(1500, 1000);
    assert(received.has_value());
    assert(received->sender == (sfu::UdpEndpoint{"127.0.0.1", 6201}));

    const auto parsed = sfu::RtpPacket::parse(received->bytes);
    assert(parsed.has_value());
    assert(parsed->sequenceNumber() == 44);
    assert(parsed->ssrc() == 123456);
    assert(parsed->payload() == std::vector<std::uint8_t>({1, 2, 3}));

    return 0;
}
