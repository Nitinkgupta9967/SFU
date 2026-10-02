#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfu {

enum class SignalType {
    Join,
    Leave,
    Offer,
    Answer,
    IceCandidate,
    Error
};

struct SignalMessage {
    SignalType type = SignalType::Error;
    std::string room_id;
    std::string participant_id;
    std::string target_participant_id;
    std::string payload;
};

struct SignalEvent {
    SignalMessage message;
    std::vector<std::string> recipients;
};

std::string signalTypeToString(SignalType type);
std::optional<SignalType> signalTypeFromString(const std::string& value);
std::optional<SignalMessage> parseSignalLine(const std::string& line);
std::string serializeSignalLine(const SignalMessage& message);

class SignalingHub {
public:
    SignalEvent handle(const SignalMessage& message);

    bool hasParticipant(const std::string& room_id, const std::string& participant_id) const;
    std::vector<std::string> participants(const std::string& room_id) const;
    std::size_t roomCount() const noexcept;

private:
    using ParticipantMap = std::unordered_map<std::string, bool>;

    SignalEvent join(const SignalMessage& message);
    SignalEvent leave(const SignalMessage& message);
    SignalEvent relay(const SignalMessage& message);
    SignalEvent error(const SignalMessage& message, const std::string& text) const;

    std::unordered_map<std::string, ParticipantMap> rooms_;
};

}  // namespace sfu
