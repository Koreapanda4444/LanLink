#pragma once
#include "lanlink/p2p/candidates.hpp"
#include <chrono>
#include <optional>
namespace lanlink::p2p {
enum class IceRole {controlling,controlled};
enum class CheckState {waiting,in_progress,succeeded,failed};
enum class ConnectivityState {checking,connected,failed};
struct CandidatePair {
    Candidate local,remote;std::uint64_t priority=0;CheckState state=CheckState::waiting;
    unsigned attempts=0;std::optional<std::uint32_t> rtt_us;
};
class ConnectivityChecks {
public:
    using Clock=std::chrono::steady_clock;
    ConnectivityChecks(auth::DeviceId local,auth::DeviceId peer,std::vector<Candidate> local_candidates,
        std::vector<Candidate> remote_candidates,Clock::time_point now=Clock::now());
    [[nodiscard]] IceRole role() const noexcept{return role_;}
    // Device IDs are the stable, authenticated tie breakers. Both agents derive the same winner.
    void resolve_role_conflict(IceRole peer_role);
    [[nodiscard]] std::optional<CandidatePair> next(Clock::time_point now=Clock::now());
    void note_probe(Endpoint local,Endpoint remote);
    void result(Endpoint local,Endpoint remote,bool success,std::optional<std::uint32_t> rtt_us={});
    [[nodiscard]] std::optional<CandidatePair> nominate(Clock::time_point now=Clock::now());
    [[nodiscard]] ConnectivityState state() const noexcept{return state_;}
    [[nodiscard]] const std::vector<CandidatePair>& pairs() const noexcept{return pairs_;}
private:
    auth::DeviceId local_,peer_;IceRole role_;std::vector<CandidatePair> pairs_;
    Clock::time_point deadline_,next_;ConnectivityState state_=ConnectivityState::checking;
};
}
