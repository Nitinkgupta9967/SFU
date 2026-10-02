#include "sfu/room.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace sfu {

Room::Room(std::string id)
    : id_(std::move(id)) {}

const std::string& Room::id() const noexcept {
    return id_;
}

bool Room::addParticipant(std::shared_ptr<Participant> participant) {
    if (!participant || participant->id().empty()) {
        return false;
    }

    const auto participant_id = participant->id();
    const auto inserted = participants_.emplace(participant_id, std::move(participant)).second;
    if (inserted) {
        activity_.emplace(
            participant_id,
            ParticipantActivity{participant_id, 0, 0, activity_tick_, 0, false});
    }

    return inserted;
}

bool Room::removeParticipant(const std::string& participant_id) {
    activity_.erase(participant_id);
    return participants_.erase(participant_id) > 0;
}

std::shared_ptr<Participant> Room::findParticipant(const std::string& participant_id) const {
    const auto found = participants_.find(participant_id);
    if (found == participants_.end()) {
        return nullptr;
    }

    return found->second;
}

std::size_t Room::participantCount() const noexcept {
    return participants_.size();
}

std::vector<std::string> Room::participantIds() const {
    std::vector<std::string> ids;
    ids.reserve(participants_.size());

    for (const auto& entry : participants_) {
        ids.push_back(entry.first);
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

ForwardingResult Room::publishRtp(const std::string& publisher_id, const RtpPacket& packet) {
    ForwardingResult result;

    if (participants_.find(publisher_id) == participants_.end()) {
        metrics_.recordDropped(publisher_id);
        result.dropped = 1;
        return result;
    }

    metrics_.recordReceived(publisher_id, packet.size());
    updatePublisherActivity(publisher_id, packet);

    for (const auto& entry : participants_) {
        const auto& subscriber = entry.second;
        if (!subscriber->wantsMediaFrom(publisher_id)) {
            continue;
        }

        subscriber->send(packet);
        metrics_.recordForwarded(subscriber->id(), packet.size());
        ++result.recipients;
    }

    return result;
}

std::optional<std::string> Room::activeSpeakerId() const {
    std::optional<std::string> active_id;
    std::uint64_t best_score = 0;
    std::uint64_t best_tick = 0;

    for (const auto& entry : activity_) {
        const auto& activity = entry.second;
        if (activity.activity_score == 0) {
            continue;
        }

        if (!active_id.has_value() ||
            activity.activity_score > best_score ||
            (activity.activity_score == best_score && activity.last_activity_tick > best_tick)) {
            active_id = activity.participant_id;
            best_score = activity.activity_score;
            best_tick = activity.last_activity_tick;
        }
    }

    return active_id;
}

std::vector<ParticipantActivity> Room::participantActivity() const {
    std::vector<ParticipantActivity> snapshot;
    snapshot.reserve(activity_.size());

    const auto active_id = activeSpeakerId();
    for (const auto& entry : activity_) {
        auto activity = entry.second;
        activity.active_speaker = active_id.has_value() && *active_id == activity.participant_id;
        snapshot.push_back(std::move(activity));
    }

    std::sort(
        snapshot.begin(),
        snapshot.end(),
        [](const ParticipantActivity& left, const ParticipantActivity& right) {
            return left.participant_id < right.participant_id;
        });

    return snapshot;
}

std::vector<std::string> Room::removeInactiveParticipants(std::uint64_t max_idle_ticks) {
    std::vector<std::string> removed;

    for (auto iterator = activity_.begin(); iterator != activity_.end();) {
        const auto idle_ticks = activity_tick_ >= iterator->second.last_activity_tick
            ? activity_tick_ - iterator->second.last_activity_tick
            : std::numeric_limits<std::uint64_t>::max();

        if (idle_ticks > max_idle_ticks) {
            const auto participant_id = iterator->first;
            participants_.erase(participant_id);
            iterator = activity_.erase(iterator);
            removed.push_back(participant_id);
        } else {
            ++iterator;
        }
    }

    std::sort(removed.begin(), removed.end());
    return removed;
}

RoomMetricsSnapshot Room::metrics() const {
    return metrics_.snapshot();
}

void Room::updatePublisherActivity(const std::string& publisher_id, const RtpPacket& packet) {
    ++activity_tick_;

    for (auto& entry : activity_) {
        entry.second.activity_score = (entry.second.activity_score * 3U) / 4U;
    }

    auto& activity = activity_[publisher_id];
    if (activity.participant_id.empty()) {
        activity.participant_id = publisher_id;
    }

    ++activity.packets_published;
    activity.bytes_published += packet.size();
    activity.last_activity_tick = activity_tick_;
    activity.activity_score += packet.size() + 1U;
}

}  // namespace sfu
