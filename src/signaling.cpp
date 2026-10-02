#include "sfu/signaling.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace sfu {

namespace {

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream input(text);

    while (std::getline(input, current, delimiter)) {
        parts.push_back(current);
    }

    return parts;
}

bool requiredFieldsPresent(const SignalMessage& message) {
    return !message.room_id.empty() && !message.participant_id.empty();
}

}  // namespace

std::string signalTypeToString(SignalType type) {
    switch (type) {
        case SignalType::Join:
            return "join";
        case SignalType::Leave:
            return "leave";
        case SignalType::Offer:
            return "offer";
        case SignalType::Answer:
            return "answer";
        case SignalType::IceCandidate:
            return "ice";
        case SignalType::Error:
            return "error";
    }

    return "error";
}

std::optional<SignalType> signalTypeFromString(const std::string& value) {
    if (value == "join") {
        return SignalType::Join;
    }
    if (value == "leave") {
        return SignalType::Leave;
    }
    if (value == "offer") {
        return SignalType::Offer;
    }
    if (value == "answer") {
        return SignalType::Answer;
    }
    if (value == "ice") {
        return SignalType::IceCandidate;
    }
    if (value == "error") {
        return SignalType::Error;
    }

    return std::nullopt;
}

std::optional<SignalMessage> parseSignalLine(const std::string& line) {
    const auto parts = split(line, '|');
    if (parts.size() < 4) {
        return std::nullopt;
    }

    const auto type = signalTypeFromString(parts[0]);
    if (!type.has_value()) {
        return std::nullopt;
    }

    SignalMessage message;
    message.type = *type;
    message.room_id = parts[1];
    message.participant_id = parts[2];
    message.target_participant_id = parts[3];

    if (parts.size() > 4) {
        message.payload = parts[4];
        for (std::size_t index = 5; index < parts.size(); ++index) {
            message.payload += '|';
            message.payload += parts[index];
        }
    }

    return message;
}

std::string serializeSignalLine(const SignalMessage& message) {
    std::ostringstream out;
    out << signalTypeToString(message.type)
        << '|'
        << message.room_id
        << '|'
        << message.participant_id
        << '|'
        << message.target_participant_id
        << '|'
        << message.payload;
    return out.str();
}

SignalEvent SignalingHub::handle(const SignalMessage& message) {
    switch (message.type) {
        case SignalType::Join:
            return join(message);
        case SignalType::Leave:
            return leave(message);
        case SignalType::Offer:
        case SignalType::Answer:
        case SignalType::IceCandidate:
            return relay(message);
        case SignalType::Error:
            return error(message, "cannot handle error as command");
    }

    return error(message, "unknown signal type");
}

bool SignalingHub::hasParticipant(
    const std::string& room_id,
    const std::string& participant_id) const {
    const auto room = rooms_.find(room_id);
    if (room == rooms_.end()) {
        return false;
    }

    return room->second.find(participant_id) != room->second.end();
}

std::vector<std::string> SignalingHub::participants(const std::string& room_id) const {
    std::vector<std::string> ids;
    const auto room = rooms_.find(room_id);
    if (room == rooms_.end()) {
        return ids;
    }

    ids.reserve(room->second.size());
    for (const auto& entry : room->second) {
        ids.push_back(entry.first);
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

std::size_t SignalingHub::roomCount() const noexcept {
    return rooms_.size();
}

SignalEvent SignalingHub::join(const SignalMessage& message) {
    if (!requiredFieldsPresent(message)) {
        return error(message, "join requires room and participant");
    }

    auto& room = rooms_[message.room_id];
    room[message.participant_id] = true;

    SignalMessage event = message;
    event.payload = "joined";

    SignalEvent result{event, participants(message.room_id)};
    result.recipients.erase(
        std::remove(result.recipients.begin(), result.recipients.end(), message.participant_id),
        result.recipients.end());
    return result;
}

SignalEvent SignalingHub::leave(const SignalMessage& message) {
    if (!requiredFieldsPresent(message)) {
        return error(message, "leave requires room and participant");
    }

    auto room = rooms_.find(message.room_id);
    if (room == rooms_.end()) {
        return SignalEvent{message, {}};
    }

    room->second.erase(message.participant_id);
    if (room->second.empty()) {
        rooms_.erase(room);
        return SignalEvent{message, {}};
    }

    return SignalEvent{message, participants(message.room_id)};
}

SignalEvent SignalingHub::relay(const SignalMessage& message) {
    if (!requiredFieldsPresent(message)) {
        return error(message, "relay requires room and participant");
    }
    if (!hasParticipant(message.room_id, message.participant_id)) {
        return error(message, "sender is not in room");
    }

    if (!message.target_participant_id.empty()) {
        if (!hasParticipant(message.room_id, message.target_participant_id)) {
            return error(message, "target is not in room");
        }

        return SignalEvent{message, {message.target_participant_id}};
    }

    auto recipients = participants(message.room_id);
    recipients.erase(
        std::remove(recipients.begin(), recipients.end(), message.participant_id),
        recipients.end());

    return SignalEvent{message, std::move(recipients)};
}

SignalEvent SignalingHub::error(const SignalMessage& message, const std::string& text) const {
    SignalMessage response;
    response.type = SignalType::Error;
    response.room_id = message.room_id;
    response.participant_id = message.participant_id;
    response.target_participant_id = message.participant_id;
    response.payload = text;
    return SignalEvent{response, {message.participant_id}};
}

}  // namespace sfu
