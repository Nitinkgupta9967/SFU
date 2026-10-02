# WebRTC Backend Integration Plan

The SFU core deliberately does not implement ICE, DTLS, or SRTP from scratch. Those protocols are security-sensitive and should be provided by a maintained WebRTC stack.

## Recommended Backend

Use libdatachannel or another C++ WebRTC stack that exposes:

- PeerConnection creation.
- SDP offer/answer handling.
- ICE candidate ingestion and emission.
- Incoming RTP packet callbacks after SRTP decrypt.
- Outgoing RTP packet send before SRTP encrypt.

## Adapter Contract

Implement `sfu::WebRtcBackend` in `include/sfu/webrtc_backend.hpp`.

Expected flow:

1. Browser joins through `ws_signaling_server`.
2. Browser sends SDP offer.
3. WebRTC backend creates a peer connection for the participant.
4. Backend returns SDP answer.
5. Browser and backend exchange ICE candidates through the signaling hub.
6. Backend emits decrypted RTP packets to `RoomRtpIngress::receiveRawRtp`.
7. Room forwards RTP packets to subscribers.
8. Backend encrypts and sends forwarded RTP packets to each browser participant.

## Remaining Hard Part

The project now links a prebuilt libdatachannel NuGet package and includes `sfu::LibDataChannelBackend`. `ws_signaling_server` routes browser offers and ICE candidates addressed to `sfu` into the backend and returns generated answers/candidates to browser clients.

The WebSocket server now registers browser participants in the media room, routes incoming backend RTP frames through `RoomRtpIngress`, and adds default outbound audio/video tracks for SFU-to-browser media. The remaining media work is hardening real-world forwarding: codec and SSRC rewriting, exact media-kind routing from negotiated SDP, and validation with two browser tabs using camera/microphone.
