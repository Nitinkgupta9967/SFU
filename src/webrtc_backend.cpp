#include "sfu/webrtc_backend.hpp"

#include "sfu/rtp_packet.hpp"

#ifdef SFU_HAS_LIBDATACHANNEL
#ifdef SFU_USE_LIBDATACHANNEL_C_API
#include <rtc/rtc.h>
#else
#include <rtc/rtc.hpp>
#endif

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
#endif

namespace sfu {

bool MissingWebRtcBackend::createParticipant(const std::string&) {
    return false;
}

bool MissingWebRtcBackend::removeParticipant(const std::string&) {
    return false;
}

WebRtcSessionDescription MissingWebRtcBackend::handleOffer(
    const std::string&,
    const WebRtcSessionDescription&) {
    return WebRtcSessionDescription{"error", reason_};
}

bool MissingWebRtcBackend::addIceCandidate(
    const std::string&,
    const WebRtcIceCandidate&) {
    return false;
}

bool MissingWebRtcBackend::sendRtp(
    const std::string&,
    const std::vector<std::uint8_t>&) {
    return false;
}

void MissingWebRtcBackend::onRtp(WebRtcRtpHandler) {}

void MissingWebRtcBackend::onLocalIceCandidate(WebRtcIceCandidateHandler) {}

const std::string& MissingWebRtcBackend::reason() const noexcept {
    return reason_;
}

#ifdef SFU_HAS_LIBDATACHANNEL
#ifdef SFU_USE_LIBDATACHANNEL_C_API
#include "webrtc_backend_capi.inc"
#else
struct LibDataChannelBackend::Impl {
    struct Session {
        std::shared_ptr<rtc::PeerConnection> peer_connection;
        std::vector<std::shared_ptr<rtc::Track>> inbound_tracks;
        std::unordered_map<std::string, std::shared_ptr<rtc::Track>> tracks_by_mid;
        std::shared_ptr<rtc::Track> outbound_audio_track;
        std::shared_ptr<rtc::Track> outbound_video_track;
        std::uint8_t outbound_audio_payload_type = 111;
        std::uint8_t outbound_video_payload_type = 96;
        std::uint32_t outbound_audio_ssrc = 0;
        std::uint32_t outbound_video_ssrc = 0;
    };

    std::mutex mutex;
    std::unordered_map<std::string, Session> sessions;
    WebRtcRtpHandler rtp_handler;
    WebRtcIceCandidateHandler ice_candidate_handler;
};

namespace {

std::string descriptionTypeToString(rtc::Description::Type type) {
    return rtc::Description::typeToString(type);
}

rtc::Description::Type descriptionTypeFromString(const std::string& type) {
    return rtc::Description::stringToType(type);
}

struct OfferedMediaSection {
    std::string type;
    std::string mid;
};

std::vector<OfferedMediaSection> offeredAudioVideoSections(const std::string& sdp) {
    std::vector<OfferedMediaSection> sections;
    std::istringstream input(sdp);
    std::string line;
    std::optional<OfferedMediaSection> current;

    auto flush = [&]() {
        if (current.has_value() && !current->mid.empty()) {
            sections.push_back(*current);
        }
        current.reset();
    };

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("m=", 0) == 0) {
            flush();
            const auto end = line.find(' ', 2);
            const auto type = line.substr(2, end == std::string::npos ? std::string::npos : end - 2);
            if (type == "audio" || type == "video") {
                current = OfferedMediaSection{type, {}};
            }
            continue;
        }

        if (current.has_value() && line.rfind("a=mid:", 0) == 0) {
            current->mid = line.substr(6);
        }
    }

    flush();
    return sections;
}

std::uint32_t participantSsrc(const std::string& participant_id, std::uint32_t salt) {
    std::uint32_t value = 2166136261U ^ salt;
    for (const auto ch : participant_id) {
        value ^= static_cast<std::uint8_t>(ch);
        value *= 16777619U;
    }
    return value == 0U ? salt : value;
}

bool sameCodec(std::string left, std::string right) {
    for (auto& ch : left) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (auto& ch : right) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return left == right;
}

std::optional<std::uint8_t> findPayloadType(const rtc::Description::Media& media, const std::string& codec) {
    for (const auto payload_type : media.payloadTypes()) {
        const auto* rtp_map = media.rtpMap(payload_type);
        if (rtp_map && sameCodec(rtp_map->format, codec) && payload_type >= 0 && payload_type <= 255) {
            return static_cast<std::uint8_t>(payload_type);
        }
    }
    return std::nullopt;
}

class IncomingRtpInterceptor final : public rtc::MediaHandler {
public:
    IncomingRtpInterceptor(std::string participant_id, std::function<void(const rtc::binary&)> on_rtp)
        : participant_id_(std::move(participant_id)),
          on_rtp_(std::move(on_rtp)) {}

    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        for (const auto& message : messages) {
            if (!message || message->type != rtc::Message::Binary || message->size() < RtpPacket::kFixedHeaderSize) {
                continue;
            }

            const auto seen = intercepted_messages_.fetch_add(1U);
            if (seen < 4U) {
                std::cerr << "webrtc media intercepted " << participant_id_
                          << " bytes=" << message->size()
                          << " rtcp=" << (rtc::IsRtcp(*message) ? "yes" : "no") << '\n';
            }

            if (rtc::IsRtcp(*message)) {
                continue;
            }

            on_rtp_(*message);
        }
    }

private:
    std::string participant_id_;
    std::function<void(const rtc::binary&)> on_rtp_;
    std::atomic<std::uint32_t> intercepted_messages_{0};
};

std::vector<std::uint8_t> toBytes(const rtc::binary& data) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(data.size());
    for (const auto byte : data) {
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    return bytes;
}

void addOutboundSsrc(
    rtc::Description::Media& media,
    std::uint32_t ssrc,
    const std::string& media_type,
    const std::string& participant_id) {
    const auto stream_id = "sfu-" + participant_id;
    const auto track_id = stream_id + "-" + media_type;
    media.addSSRC(ssrc, "sfu-" + media_type, stream_id, track_id);
}

std::string sanitizeAnswerSdp(std::string sdp) {
    const std::array<std::string, 2> prefixes{"m=audio 0 ", "m=video 0 "};
    for (const auto& prefix : prefixes) {
        std::size_t position = 0;
        while ((position = sdp.find(prefix, position)) != std::string::npos) {
            sdp[position + prefix.size() - 2U] = '9';
            position += prefix.size();
        }
    }
    return sdp;
}

bool isAudioOrVideoSection(const std::string& line) {
    return line.rfind("m=audio ", 0) == 0 || line.rfind("m=video ", 0) == 0;
}

std::string cleanAnswerSsrcsAndMsids(const std::string& sdp) {
    std::istringstream input(sdp);
    std::ostringstream output;
    std::string line;
    bool in_audio_or_video = false;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("m=", 0) == 0) {
            in_audio_or_video = isAudioOrVideoSection(line);
        }

        if (in_audio_or_video) {
            if (line.rfind("a=ssrc-group:", 0) == 0) {
                continue;
            }
            if (line.rfind("a=msid:", 0) == 0 && line.rfind("a=msid:sfu-", 0) != 0) {
                continue;
            }
            if (line.rfind("a=ssrc:", 0) == 0 &&
                line.find(" cname:sfu-") == std::string::npos &&
                line.find(" msid:sfu-") == std::string::npos) {
                continue;
            }
        }

        output << line << "\r\n";
    }

    return output.str();
}

std::string prepareAnswerSdp(std::string sdp) {
    return cleanAnswerSsrcsAndMsids(sanitizeAnswerSdp(std::move(sdp)));
}

}  // namespace

LibDataChannelBackend::LibDataChannelBackend()
    : impl_(std::make_unique<Impl>()) {}

LibDataChannelBackend::~LibDataChannelBackend() = default;

bool LibDataChannelBackend::createParticipant(const std::string& participant_id) {
    if (participant_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sessions.find(participant_id) != impl_->sessions.end()) {
        return true;
    }

    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    config.disableAutoNegotiation = true;

    auto peer_connection = std::make_shared<rtc::PeerConnection>(config);
    peer_connection->onStateChange(
        [participant_id](rtc::PeerConnection::State state) {
            std::cerr << "webrtc state " << participant_id << ' ' << state << '\n';
        });
    peer_connection->onIceStateChange(
        [participant_id](rtc::PeerConnection::IceState state) {
            std::cerr << "webrtc ice " << participant_id << ' ' << state << '\n';
        });

    peer_connection->onLocalCandidate(
        [this, participant_id](rtc::Candidate candidate) {
            try {
                WebRtcIceCandidateHandler handler;
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    handler = impl_->ice_candidate_handler;
                }

                if (handler) {
                    WebRtcIceCandidate ice;
                    ice.candidate = candidate.candidate();
                    ice.mid = candidate.mid();
                    handler(participant_id, ice);
                }
            } catch (const std::exception& error) {
                std::cerr << "webrtc local ICE error " << participant_id
                          << " error=" << error.what() << '\n';
            } catch (...) {
                std::cerr << "webrtc local ICE error " << participant_id
                          << " error=unknown\n";
            }
        });

    peer_connection->onTrack(
        [this, participant_id](std::shared_ptr<rtc::Track> track) {
            try {
                const auto mid = track->mid();
                auto description = track->description();
                const auto media_type = description.type();
                std::optional<std::uint8_t> outbound_payload_type;
                std::uint32_t outbound_ssrc = 0;

                const auto direction = description.direction();
                const bool may_send_back =
                    direction != rtc::Description::Direction::RecvOnly &&
                    direction != rtc::Description::Direction::Inactive;

                if (media_type == "audio" && may_send_back) {
                    outbound_payload_type = findPayloadType(description, "opus");
                    outbound_ssrc = participantSsrc(participant_id, 0xA0D10U);
                    if (outbound_payload_type.has_value()) {
                        description.setDirection(rtc::Description::Direction::RecvOnly);
                        track->setDescription(std::move(description));
                    }
                } else if (media_type == "video" && may_send_back) {
                    outbound_payload_type = findPayloadType(description, "VP8");
                    outbound_ssrc = participantSsrc(participant_id, 0xB1D30U);
                    if (outbound_payload_type.has_value()) {
                        description.setDirection(rtc::Description::Direction::RecvOnly);
                        track->setDescription(std::move(description));
                    }
                }

                if (!outbound_payload_type.has_value()) {
                    std::cerr << "webrtc track negotiated receive-only " << participant_id
                              << " mid=" << mid
                              << " media=" << media_type << '\n';
                }

                track->setMediaHandler(
                    std::make_shared<IncomingRtpInterceptor>(
                        participant_id,
                        [this, participant_id](const rtc::binary& data) {
                            try {
                                WebRtcRtpHandler handler;
                                {
                                    std::lock_guard<std::mutex> lock(impl_->mutex);
                                    handler = impl_->rtp_handler;
                                }

                                if (handler) {
                                    handler(participant_id, toBytes(data));
                                }
                            } catch (const std::exception& error) {
                                std::cerr << "webrtc RTP intercept error " << participant_id
                                          << " error=" << error.what() << '\n';
                            } catch (...) {
                                std::cerr << "webrtc RTP intercept error " << participant_id
                                          << " error=unknown\n";
                            }
                        }));
                track->onOpen(
                    [participant_id, mid]() {
                        std::cerr << "webrtc track open " << participant_id << " mid=" << mid << '\n';
                    });
                track->onClosed(
                    [participant_id, mid]() {
                        std::cerr << "webrtc track closed " << participant_id << " mid=" << mid << '\n';
                    });
                track->onError(
                    [participant_id, mid](std::string error) {
                        std::cerr << "webrtc track error " << participant_id << " mid=" << mid
                                  << " error=" << error << '\n';
                    });
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    auto found = impl_->sessions.find(participant_id);
                    if (found != impl_->sessions.end()) {
                        found->second.inbound_tracks.push_back(track);
                        found->second.tracks_by_mid[track->mid()] = track;
                        if (media_type == "audio" && outbound_payload_type.has_value()) {
                            found->second.outbound_audio_track = track;
                            found->second.outbound_audio_payload_type = *outbound_payload_type;
                            found->second.outbound_audio_ssrc = outbound_ssrc;
                        } else if (media_type == "video" && outbound_payload_type.has_value()) {
                            found->second.outbound_video_track = track;
                            found->second.outbound_video_payload_type = *outbound_payload_type;
                            found->second.outbound_video_ssrc = outbound_ssrc;
                        }
                    }
                }

                track->onMessage(
                    [this, participant_id](rtc::binary data) {
                        try {
                            WebRtcRtpHandler handler;
                            {
                                std::lock_guard<std::mutex> lock(impl_->mutex);
                                handler = impl_->rtp_handler;
                            }

                            if (handler) {
                                handler(participant_id, toBytes(data));
                            }
                        } catch (const std::exception& error) {
                            std::cerr << "webrtc RTP receive error " << participant_id
                                      << " error=" << error.what() << '\n';
                        } catch (...) {
                            std::cerr << "webrtc RTP receive error " << participant_id
                                      << " error=unknown\n";
                        }
                    },
                    nullptr);
            } catch (const std::exception& error) {
                std::cerr << "webrtc track setup failed " << participant_id
                          << " error=" << error.what() << '\n';
            } catch (...) {
                std::cerr << "webrtc track setup failed " << participant_id
                          << " error=unknown\n";
            }
        });

    impl_->sessions.emplace(
        participant_id,
        Impl::Session{peer_connection, {}, {}, nullptr, nullptr});
    return true;
}

bool LibDataChannelBackend::removeParticipant(const std::string& participant_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sessions.erase(participant_id) > 0;
}

WebRtcSessionDescription LibDataChannelBackend::handleOffer(
    const std::string& participant_id,
    const WebRtcSessionDescription& offer) {
    if (offer.sdp.empty()) {
        return WebRtcSessionDescription{"error", "empty SDP offer"};
    }

    createParticipant(participant_id);

    std::shared_ptr<rtc::PeerConnection> peer_connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->sessions.find(participant_id);
        if (found == impl_->sessions.end()) {
            return WebRtcSessionDescription{"error", "participant not found"};
        }
        peer_connection = found->second.peer_connection;
    }

    struct AnswerState {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<WebRtcSessionDescription> answer;
    };
    auto answer_state = std::make_shared<AnswerState>();
    const auto publish_answer =
        [answer_state](const rtc::Description& description) {
            if (description.type() != rtc::Description::Type::Answer) {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(answer_state->mutex);
                answer_state->answer = WebRtcSessionDescription{
                    descriptionTypeToString(description.type()),
                    prepareAnswerSdp(static_cast<std::string>(description))};
            }
            answer_state->ready.notify_one();
        };

    peer_connection->onLocalDescription(
        [publish_answer](rtc::Description description) {
            publish_answer(description);
        });

    peer_connection->onGatheringStateChange(
        [peer_connection, publish_answer](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                const auto description = peer_connection->localDescription();
                if (description.has_value()) {
                    publish_answer(*description);
                }
            }
        });

    try {
        peer_connection->setRemoteDescription(
            rtc::Description(offer.sdp, descriptionTypeFromString(offer.type.empty() ? "offer" : offer.type)));

        peer_connection->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception& error) {
        return WebRtcSessionDescription{"error", error.what()};
    } catch (...) {
        return WebRtcSessionDescription{"error", "unknown WebRTC negotiation error"};
    }

    std::unique_lock<std::mutex> lock(answer_state->mutex);
    if (!answer_state->ready.wait_for(
            lock,
            std::chrono::seconds(5),
            [&]() { return answer_state->answer.has_value(); })) {
        return WebRtcSessionDescription{"error", "timed out waiting for SDP answer"};
    }

    return *answer_state->answer;
}

bool LibDataChannelBackend::addIceCandidate(
    const std::string& participant_id,
    const WebRtcIceCandidate& candidate) {
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->sessions.find(participant_id);
        if (found == impl_->sessions.end()) {
            return false;
        }
        peer_connection = found->second.peer_connection;
    }

    try {
        peer_connection->addRemoteCandidate(rtc::Candidate(candidate.candidate, candidate.mid));
    } catch (const std::exception& error) {
        std::cerr << "webrtc add ICE failed " << participant_id
                  << " error=" << error.what() << '\n';
        return false;
    } catch (...) {
        std::cerr << "webrtc add ICE failed " << participant_id
                  << " error=unknown\n";
        return false;
    }
    return true;
}

bool LibDataChannelBackend::sendRtp(
    const std::string& participant_id,
    const std::vector<std::uint8_t>& rtp_packet) {
    std::shared_ptr<rtc::Track> track;
    std::uint8_t payload_type = 96;
    std::uint32_t ssrc = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->sessions.find(participant_id);
        if (found == impl_->sessions.end()) {
            return false;
        }

        const auto parsed = RtpPacket::parse(rtp_packet);
        if (parsed.has_value() && parsed->mediaKind() == MediaKind::Audio) {
            track = found->second.outbound_audio_track;
            payload_type = found->second.outbound_audio_payload_type;
            ssrc = found->second.outbound_audio_ssrc;
        } else {
            track = found->second.outbound_video_track;
            payload_type = found->second.outbound_video_payload_type;
            ssrc = found->second.outbound_video_ssrc;
        }
    }

    if (track && track->isOpen() && ssrc != 0U) {
        auto rewritten = rtp_packet;
        if (rewritten.size() >= 12U) {
            rewritten[1] = static_cast<std::uint8_t>((rewritten[1] & 0x80U) | (payload_type & 0x7fU));
            rewritten[8] = static_cast<std::uint8_t>((ssrc >> 24U) & 0xffU);
            rewritten[9] = static_cast<std::uint8_t>((ssrc >> 16U) & 0xffU);
            rewritten[10] = static_cast<std::uint8_t>((ssrc >> 8U) & 0xffU);
            rewritten[11] = static_cast<std::uint8_t>(ssrc & 0xffU);
        }
        try {
            return track->send(reinterpret_cast<const rtc::byte*>(rewritten.data()), rewritten.size());
        } catch (const std::exception& error) {
            std::cerr << "webrtc RTP send failed " << participant_id
                      << " error=" << error.what() << '\n';
        } catch (...) {
            std::cerr << "webrtc RTP send failed " << participant_id
                      << " error=unknown\n";
        }
    }

    return false;
}

void LibDataChannelBackend::onRtp(WebRtcRtpHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->rtp_handler = std::move(handler);
}

void LibDataChannelBackend::onLocalIceCandidate(WebRtcIceCandidateHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ice_candidate_handler = std::move(handler);
}
#endif  // SFU_USE_LIBDATACHANNEL_C_API
#endif

}  // namespace sfu
