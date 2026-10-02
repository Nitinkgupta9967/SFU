#include "sfu/signaling.hpp"

#include <cassert>
#include <vector>

namespace {

void parsesAndSerializesSignalLines() {
    const auto parsed = sfu::parseSignalLine("offer|room-a|alice|bob|fake-sdp|with-pipe");
    assert(parsed.has_value());
    assert(parsed->type == sfu::SignalType::Offer);
    assert(parsed->room_id == "room-a");
    assert(parsed->participant_id == "alice");
    assert(parsed->target_participant_id == "bob");
    assert(parsed->payload == "fake-sdp|with-pipe");

    assert(sfu::serializeSignalLine(*parsed) == "offer|room-a|alice|bob|fake-sdp|with-pipe");
    assert(!sfu::parseSignalLine("broken").has_value());
}

void joinLeaveTracksParticipants() {
    sfu::SignalingHub hub;

    const auto first_join = hub.handle({sfu::SignalType::Join, "room-a", "alice", "", ""});
    assert(first_join.recipients.empty());
    assert(hub.hasParticipant("room-a", "alice"));

    const auto second_join = hub.handle({sfu::SignalType::Join, "room-a", "bob", "", ""});
    assert(second_join.recipients == std::vector<std::string>({"alice"}));
    assert(hub.participants("room-a") == std::vector<std::string>({"alice", "bob"}));

    const auto leave = hub.handle({sfu::SignalType::Leave, "room-a", "bob", "", ""});
    assert(leave.recipients == std::vector<std::string>({"alice"}));
    assert(!hub.hasParticipant("room-a", "bob"));
}

void relayRoutesToTargetOrRoom() {
    sfu::SignalingHub hub;
    hub.handle({sfu::SignalType::Join, "room-a", "alice", "", ""});
    hub.handle({sfu::SignalType::Join, "room-a", "bob", "", ""});
    hub.handle({sfu::SignalType::Join, "room-a", "carol", "", ""});

    const auto targeted = hub.handle({sfu::SignalType::Offer, "room-a", "alice", "bob", "sdp"});
    assert(targeted.recipients == std::vector<std::string>({"bob"}));

    const auto broadcast = hub.handle({sfu::SignalType::IceCandidate, "room-a", "alice", "", "candidate"});
    assert(broadcast.recipients == std::vector<std::string>({"bob", "carol"}));
}

void rejectsUnknownSendersAndTargets() {
    sfu::SignalingHub hub;
    hub.handle({sfu::SignalType::Join, "room-a", "alice", "", ""});

    const auto unknown_sender = hub.handle({sfu::SignalType::Offer, "room-a", "missing", "alice", "sdp"});
    assert(unknown_sender.message.type == sfu::SignalType::Error);

    const auto unknown_target = hub.handle({sfu::SignalType::Offer, "room-a", "alice", "missing", "sdp"});
    assert(unknown_target.message.type == sfu::SignalType::Error);
}

}  // namespace

int main() {
    parsesAndSerializesSignalLines();
    joinLeaveTracksParticipants();
    relayRoutesToTargetOrRoom();
    rejectsUnknownSendersAndTargets();
    return 0;
}
