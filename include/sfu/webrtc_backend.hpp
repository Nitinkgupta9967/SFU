#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sfu {

struct WebRtcSessionDescription {
    std::string type;
    std::string sdp;
};

struct WebRtcIceCandidate {
    std::string candidate;
    std::string mid;
    std::uint32_t mline_index = 0;
};

using WebRtcRtpHandler = std::function<void(
    const std::string& participant_id,
    const std::vector<std::uint8_t>& rtp_packet)>;

using WebRtcIceCandidateHandler = std::function<void(
    const std::string& participant_id,
    const WebRtcIceCandidate& candidate)>;

class WebRtcBackend {
public:
    virtual ~WebRtcBackend() = default;

    virtual bool createParticipant(const std::string& participant_id) = 0;
    virtual bool removeParticipant(const std::string& participant_id) = 0;
    virtual WebRtcSessionDescription handleOffer(
        const std::string& participant_id,
        const WebRtcSessionDescription& offer) = 0;
    virtual bool addIceCandidate(
        const std::string& participant_id,
        const WebRtcIceCandidate& candidate) = 0;
    virtual bool sendRtp(
        const std::string& participant_id,
        const std::vector<std::uint8_t>& rtp_packet) = 0;
    virtual void onRtp(WebRtcRtpHandler handler) = 0;
    virtual void onLocalIceCandidate(WebRtcIceCandidateHandler handler) = 0;
};

class MissingWebRtcBackend final : public WebRtcBackend {
public:
    bool createParticipant(const std::string& participant_id) override;
    bool removeParticipant(const std::string& participant_id) override;
    WebRtcSessionDescription handleOffer(
        const std::string& participant_id,
        const WebRtcSessionDescription& offer) override;
    bool addIceCandidate(
        const std::string& participant_id,
        const WebRtcIceCandidate& candidate) override;
    bool sendRtp(
        const std::string& participant_id,
        const std::vector<std::uint8_t>& rtp_packet) override;
    void onRtp(WebRtcRtpHandler handler) override;
    void onLocalIceCandidate(WebRtcIceCandidateHandler handler) override;

    const std::string& reason() const noexcept;

private:
    std::string reason_ =
        "No WebRTC media backend is linked. Install and integrate libdatachannel or another ICE/DTLS/SRTP stack.";
};

#ifdef SFU_HAS_LIBDATACHANNEL
class LibDataChannelBackend final : public WebRtcBackend {
public:
    LibDataChannelBackend();
    ~LibDataChannelBackend() override;

    bool createParticipant(const std::string& participant_id) override;
    bool removeParticipant(const std::string& participant_id) override;
    WebRtcSessionDescription handleOffer(
        const std::string& participant_id,
        const WebRtcSessionDescription& offer) override;
    bool addIceCandidate(
        const std::string& participant_id,
        const WebRtcIceCandidate& candidate) override;
    bool sendRtp(
        const std::string& participant_id,
        const std::vector<std::uint8_t>& rtp_packet) override;
    void onRtp(WebRtcRtpHandler handler) override;
    void onLocalIceCandidate(WebRtcIceCandidateHandler handler) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

}  // namespace sfu
