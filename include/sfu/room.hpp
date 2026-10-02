#pragma once

#include "sfu/metrics.hpp"
#include "sfu/participant.hpp"
#include "sfu/rtp_packet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfu {

struct ForwardingResult {
    std::size_t recipients = 0;
    std::size_t dropped = 0;
};

struct ParticipantActivity {
    std::string participant_id;
    std::uint64_t packets_published = 0;
    std::uint64_t bytes_published = 0;
    std::uint64_t last_activity_tick = 0;
    std::uint64_t activity_score = 0;
    bool active_speaker = false;
};

class Room {
public:
    explicit Room(std::string id);

    const std::string& id() const noexcept;

    bool addParticipant(std::shared_ptr<Participant> participant);
    bool removeParticipant(const std::string& participant_id);
    std::shared_ptr<Participant> findParticipant(const std::string& participant_id) const;

    std::size_t participantCount() const noexcept;
    std::vector<std::string> participantIds() const;

    ForwardingResult publishRtp(const std::string& publisher_id, const RtpPacket& packet);
    std::optional<std::string> activeSpeakerId() const;
    std::vector<ParticipantActivity> participantActivity() const;
    std::vector<std::string> removeInactiveParticipants(std::uint64_t max_idle_ticks);

    RoomMetricsSnapshot metrics() const;

private:
    void updatePublisherActivity(const std::string& publisher_id, const RtpPacket& packet);

    std::string id_;
    std::unordered_map<std::string, std::shared_ptr<Participant>> participants_;
    std::unordered_map<std::string, ParticipantActivity> activity_;
    MetricsRegistry metrics_;
    std::uint64_t activity_tick_ = 0;
};

}  // namespace sfu
