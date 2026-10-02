namespace {

std::uint32_t participantSsrc(const std::string& participant_id, std::uint32_t salt) {
    std::uint32_t value = 2166136261U ^ salt;
    for (const auto ch : participant_id) {
        value ^= static_cast<std::uint8_t>(ch);
        value *= 16777619U;
    }
    return value == 0U ? salt : value;
}

std::string activateMediaPorts(std::string sdp) {
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

const char* stateName(rtcState state) {
    switch (state) {
    case RTC_NEW: return "new";
    case RTC_CONNECTING: return "connecting";
    case RTC_CONNECTED: return "connected";
    case RTC_DISCONNECTED: return "disconnected";
    case RTC_FAILED: return "failed";
    case RTC_CLOSED: return "closed";
    }
    return "unknown";
}

const char* iceStateName(rtcIceState state) {
    switch (state) {
    case RTC_ICE_NEW: return "new";
    case RTC_ICE_CHECKING: return "checking";
    case RTC_ICE_CONNECTED: return "connected";
    case RTC_ICE_COMPLETED: return "completed";
    case RTC_ICE_FAILED: return "failed";
    case RTC_ICE_DISCONNECTED: return "disconnected";
    case RTC_ICE_CLOSED: return "closed";
    }
    return "unknown";
}

std::string readTrackMid(int track) {
    char buffer[64] = {};
    return rtcGetTrackMid(track, buffer, static_cast<int>(sizeof(buffer))) >= 0
        ? std::string(buffer)
        : std::string{};
}

std::string readTrackDescription(int track) {
    const auto required = rtcGetTrackDescription(track, nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::string description(static_cast<std::size_t>(required), '\0');
    if (rtcGetTrackDescription(track, &description[0], required) < 0) {
        return {};
    }
    description.resize(std::strlen(description.c_str()));
    return description;
}

std::string mediaTypeFromDescription(const std::string& description) {
    if (description.rfind("m=audio ", 0) == 0 || description.find("\nm=audio ") != std::string::npos) {
        return "audio";
    }
    if (description.rfind("m=video ", 0) == 0 || description.find("\nm=video ") != std::string::npos) {
        return "video";
    }
    return {};
}

std::uint8_t firstPayloadTypeForCodec(int track, const char* codec, std::uint8_t fallback) {
    int payload_types[8] = {};
    const auto count = rtcGetTrackPayloadTypesForCodec(track, codec, payload_types, 8);
    if (count > 0 && payload_types[0] >= 0 && payload_types[0] <= 127) {
        return static_cast<std::uint8_t>(payload_types[0]);
    }
    return fallback;
}

}  // namespace

struct LibDataChannelBackend::Impl {
    struct AnswerState {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<WebRtcSessionDescription> answer;
    };

    struct Session {
        int peer_connection = -1;
        int outbound_audio_track = -1;
        int outbound_video_track = -1;
        std::uint8_t outbound_audio_payload_type = 111;
        std::uint8_t outbound_video_payload_type = 96;
        std::uint32_t outbound_audio_ssrc = 0;
        std::uint32_t outbound_video_ssrc = 0;
        std::shared_ptr<AnswerState> answer_state;
    };

    std::mutex mutex;
    std::unordered_map<std::string, Session> sessions;
    std::unordered_map<int, std::string> participants_by_pc;
    std::unordered_map<int, std::string> participants_by_track;
    WebRtcRtpHandler rtp_handler;
    WebRtcIceCandidateHandler ice_candidate_handler;

    std::optional<std::string> participantForPc(int pc) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = participants_by_pc.find(pc);
        return found == participants_by_pc.end() ? std::nullopt : std::optional<std::string>(found->second);
    }

    std::optional<std::string> participantForTrack(int track) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = participants_by_track.find(track);
        return found == participants_by_track.end() ? std::nullopt : std::optional<std::string>(found->second);
    }

    static void RTC_API onLocalDescription(int pc, const char* sdp, const char* type, void* ptr) {
        if (!ptr || !type || std::string(type) != "answer") {
            return;
        }

        auto& self = *static_cast<Impl*>(ptr);
        std::shared_ptr<AnswerState> answer_state;
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            const auto participant = self.participants_by_pc.find(pc);
            if (participant == self.participants_by_pc.end()) {
                return;
            }
            const auto session = self.sessions.find(participant->second);
            if (session == self.sessions.end()) {
                return;
            }
            answer_state = session->second.answer_state;
        }

        if (!answer_state) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(answer_state->mutex);
            answer_state->answer = WebRtcSessionDescription{
                type,
                activateMediaPorts(sdp ? std::string(sdp) : std::string{})};
        }
        answer_state->ready.notify_one();
    }

    static void RTC_API onLocalCandidate(int pc, const char* cand, const char* mid, void* ptr) {
        if (!ptr || !cand) {
            return;
        }

        auto& self = *static_cast<Impl*>(ptr);
        WebRtcIceCandidateHandler handler;
        std::string participant_id;
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            const auto participant = self.participants_by_pc.find(pc);
            if (participant == self.participants_by_pc.end()) {
                return;
            }
            participant_id = participant->second;
            handler = self.ice_candidate_handler;
        }

        if (handler) {
            WebRtcIceCandidate candidate;
            candidate.candidate = cand;
            candidate.mid = mid ? mid : "";
            handler(participant_id, candidate);
        }
    }

    static void RTC_API onStateChange(int pc, rtcState state, void* ptr) {
        if (!ptr) {
            return;
        }
        const auto participant = static_cast<Impl*>(ptr)->participantForPc(pc);
        if (participant.has_value()) {
            std::cerr << "webrtc state " << *participant << ' ' << stateName(state) << '\n';
        }
    }

    static void RTC_API onIceStateChange(int pc, rtcIceState state, void* ptr) {
        if (!ptr) {
            return;
        }
        const auto participant = static_cast<Impl*>(ptr)->participantForPc(pc);
        if (participant.has_value()) {
            std::cerr << "webrtc ice " << *participant << ' ' << iceStateName(state) << '\n';
        }
    }

    static void RTC_API onTrack(int pc, int track, void* ptr) {
        if (!ptr || track < 0) {
            return;
        }

        auto& self = *static_cast<Impl*>(ptr);
        const auto participant = self.participantForPc(pc);
        if (!participant.has_value()) {
            return;
        }

        const auto participant_id = *participant;
        const auto mid = readTrackMid(track);
        const auto media_type = mediaTypeFromDescription(readTrackDescription(track));

        rtcSetUserPointer(track, ptr);
        rtcChainRtcpReceivingSession(track);
        rtcSetMessageCallback(track, &Impl::onTrackMessage);
        rtcSetOpenCallback(track, &Impl::onTrackOpen);
        rtcSetClosedCallback(track, &Impl::onTrackClosed);
        rtcSetErrorCallback(track, &Impl::onTrackError);

        {
            std::lock_guard<std::mutex> lock(self.mutex);
            self.participants_by_track[track] = participant_id;
            auto session = self.sessions.find(participant_id);
            if (session != self.sessions.end()) {
                if (media_type == "audio" || mid == "0") {
                    session->second.outbound_audio_track = track;
                    session->second.outbound_audio_payload_type = firstPayloadTypeForCodec(track, "opus", 111);
                    session->second.outbound_audio_ssrc = participantSsrc(participant_id, 0xA0D10U);
                } else if (media_type == "video" || mid == "1") {
                    session->second.outbound_video_track = track;
                    session->second.outbound_video_payload_type = firstPayloadTypeForCodec(track, "VP8", 96);
                    session->second.outbound_video_ssrc = participantSsrc(participant_id, 0xB1D30U);
                }
            }
        }
    }

    static void RTC_API onTrackOpen(int track, void* ptr) {
        if (!ptr) {
            return;
        }
        const auto participant = static_cast<Impl*>(ptr)->participantForTrack(track);
        if (participant.has_value()) {
            std::cerr << "webrtc track open " << *participant << " id=" << track << '\n';
        }
    }

    static void RTC_API onTrackClosed(int track, void* ptr) {
        if (!ptr) {
            return;
        }
        const auto participant = static_cast<Impl*>(ptr)->participantForTrack(track);
        if (participant.has_value()) {
            std::cerr << "webrtc track closed " << *participant << " id=" << track << '\n';
        }
    }

    static void RTC_API onTrackError(int track, const char* error, void* ptr) {
        if (!ptr) {
            return;
        }
        const auto participant = static_cast<Impl*>(ptr)->participantForTrack(track);
        if (participant.has_value()) {
            std::cerr << "webrtc track error " << *participant << " id=" << track
                      << " error=" << (error ? error : "") << '\n';
        }
    }

    static void RTC_API onTrackMessage(int track, const char* message, int size, void* ptr) {
        if (!ptr || !message || size <= 0) {
            return;
        }

        auto& self = *static_cast<Impl*>(ptr);
        const auto participant = self.participantForTrack(track);
        if (!participant.has_value()) {
            return;
        }

        WebRtcRtpHandler handler;
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            handler = self.rtp_handler;
        }

        if (handler) {
            const auto* first = reinterpret_cast<const std::uint8_t*>(message);
            handler(*participant, std::vector<std::uint8_t>(first, first + size));
        }
    }
};

LibDataChannelBackend::LibDataChannelBackend()
    : impl_(std::make_unique<Impl>()) {
    rtcInitLogger(RTC_LOG_WARNING, nullptr);
}

LibDataChannelBackend::~LibDataChannelBackend() = default;

bool LibDataChannelBackend::createParticipant(const std::string& participant_id) {
    if (participant_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->sessions.find(participant_id) != impl_->sessions.end()) {
        return true;
    }

    const char* ice_servers[] = {"stun:stun.l.google.com:19302"};
    rtcConfiguration config{};
    config.iceServers = ice_servers;
    config.iceServersCount = 1;
    config.disableAutoNegotiation = true;

    const auto pc = rtcCreatePeerConnection(&config);
    if (pc < 0) {
        return false;
    }

    rtcSetUserPointer(pc, impl_.get());
    rtcSetLocalDescriptionCallback(pc, &Impl::onLocalDescription);
    rtcSetLocalCandidateCallback(pc, &Impl::onLocalCandidate);
    rtcSetStateChangeCallback(pc, &Impl::onStateChange);
    rtcSetIceStateChangeCallback(pc, &Impl::onIceStateChange);
    rtcSetTrackCallback(pc, &Impl::onTrack);

    Impl::Session session;
    session.peer_connection = pc;
    impl_->participants_by_pc[pc] = participant_id;
    impl_->sessions.emplace(participant_id, std::move(session));
    return true;
}

bool LibDataChannelBackend::removeParticipant(const std::string& participant_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->sessions.find(participant_id);
    if (found == impl_->sessions.end()) {
        return false;
    }

    if (found->second.outbound_audio_track >= 0) {
        impl_->participants_by_track.erase(found->second.outbound_audio_track);
        rtcDeleteTrack(found->second.outbound_audio_track);
    }
    if (found->second.outbound_video_track >= 0) {
        impl_->participants_by_track.erase(found->second.outbound_video_track);
        rtcDeleteTrack(found->second.outbound_video_track);
    }
    impl_->participants_by_pc.erase(found->second.peer_connection);
    rtcClosePeerConnection(found->second.peer_connection);
    rtcDeletePeerConnection(found->second.peer_connection);
    impl_->sessions.erase(found);
    return true;
}

WebRtcSessionDescription LibDataChannelBackend::handleOffer(
    const std::string& participant_id,
    const WebRtcSessionDescription& offer) {
    if (offer.sdp.empty()) {
        return WebRtcSessionDescription{"error", "empty SDP offer"};
    }

    createParticipant(participant_id);

    int pc = -1;
    auto answer_state = std::make_shared<Impl::AnswerState>();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->sessions.find(participant_id);
        if (found == impl_->sessions.end()) {
            return WebRtcSessionDescription{"error", "participant not found"};
        }
        pc = found->second.peer_connection;
        found->second.answer_state = answer_state;
    }

    if (rtcSetRemoteDescription(pc, offer.sdp.c_str(), offer.type.empty() ? "offer" : offer.type.c_str()) < 0) {
        return WebRtcSessionDescription{"error", "failed to set remote description"};
    }
    if (rtcSetLocalDescription(pc, "answer") < 0) {
        return WebRtcSessionDescription{"error", "failed to set local answer"};
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
    int pc = -1;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(participant_id);
        if (found == impl_->sessions.end()) {
            return false;
        }
        pc = found->second.peer_connection;
    }

    return rtcAddRemoteCandidate(pc, candidate.candidate.c_str(), candidate.mid.c_str()) >= 0;
}

bool LibDataChannelBackend::sendRtp(
    const std::string& participant_id,
    const std::vector<std::uint8_t>& rtp_packet) {
    int track = -1;
    std::uint8_t payload_type = 96;
    std::uint32_t ssrc = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->sessions.find(participant_id);
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

    if (track < 0 || ssrc == 0U || !rtcIsOpen(track)) {
        return false;
    }

    auto rewritten = rtp_packet;
    if (rewritten.size() >= 12U) {
        rewritten[1] = static_cast<std::uint8_t>((rewritten[1] & 0x80U) | (payload_type & 0x7fU));
        rewritten[8] = static_cast<std::uint8_t>((ssrc >> 24U) & 0xffU);
        rewritten[9] = static_cast<std::uint8_t>((ssrc >> 16U) & 0xffU);
        rewritten[10] = static_cast<std::uint8_t>((ssrc >> 8U) & 0xffU);
        rewritten[11] = static_cast<std::uint8_t>(ssrc & 0xffU);
    }

    return rtcSendMessage(
        track,
        reinterpret_cast<const char*>(rewritten.data()),
        static_cast<int>(rewritten.size())) >= 0;
}

void LibDataChannelBackend::onRtp(WebRtcRtpHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->rtp_handler = std::move(handler);
}

void LibDataChannelBackend::onLocalIceCandidate(WebRtcIceCandidateHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ice_candidate_handler = std::move(handler);
}
