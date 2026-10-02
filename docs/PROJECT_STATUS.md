# SFU Project Status

## Implemented

- C++17/CMake project layout.
- RTP packet parse, serialize, and media-kind helpers.
- Room and participant model.
- One-to-many RTP forwarding.
- Per-room and per-participant metrics.
- Raw RTP transport boundary.
- UDP RTP transport and deterministic loopback demo.
- RTP-activity based active speaker tracking.
- Idle participant removal API.
- Signaling hub for join, leave, offer, answer, and ICE candidate routing.
- TCP line-protocol signaling server and client.
- WebSocket signaling bridge for browser tabs.
- Browser signaling demo in `web/`.
- Automated tests for core, UDP transport, UDP SFU integration, and signaling.
- Prebuilt libdatachannel NuGet package wired into CMake.
- `LibDataChannelBackend` adapter for PeerConnection creation, SDP answer generation, ICE candidate ingestion, incoming track frame callbacks, and RTP send attempts.
- WebSocket server routes browser SDP offers and ICE candidates addressed to `sfu` into `LibDataChannelBackend`.
- Browser-origin media frames from libdatachannel are injected into `RoomRtpIngress`.
- Browser participants are registered in the media `Room` and receive forwarded RTP through backend send callbacks.
- Default outbound audio/video tracks are added for browser participants so answers can negotiate SFU-to-browser media sections.

## Not Yet Implemented

- End-to-end browser media validation across two tabs with real camera/microphone.
- Codec/SSRC rewriting for robust multi-party forwarding.
- Simulcast/RID layer selection.
- REMB/TWCC bandwidth adaptation.
- Audio-level RTP extension active speaker detection.

## Completion Estimate

The project is roughly 75% of a WebRTC SFU MVP.

The SFU core, UDP media forwarding proof, signaling model, TCP signaling, browser WebSocket signaling bridge, prebuilt WebRTC backend linkage, browser offer/ICE routing, browser RTP ingress, and default outbound media tracks are implemented. The largest remaining work is real two-tab browser validation and hardening codec/SSRC rewriting.
