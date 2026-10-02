#include "sfu/webrtc_backend.hpp"

#include <rtc/rtc.hpp>

#include <iostream>

int main() {
    rtc::Configuration config;
    rtc::PeerConnection peer_connection(config);
    peer_connection.onStateChange([](rtc::PeerConnection::State) {});

#ifdef SFU_HAS_LIBDATACHANNEL
    sfu::LibDataChannelBackend backend;
    if (!backend.createParticipant("alice")) {
        return 1;
    }
    if (!backend.removeParticipant("alice")) {
        return 1;
    }
#endif

    std::cout << "libdatachannel smoke ok\n";
    return 0;
}
