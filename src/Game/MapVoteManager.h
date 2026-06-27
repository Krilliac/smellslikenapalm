// src/Game/MapVoteManager.h – End-of-round map voting

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

class MapConfig;

// Manages the end-of-round map vote described in docs/MAPS.md §7:
//   * a weighted random selection of candidate maps (excluding the current map),
//   * per-client vote casting,
//   * tallying with ties broken by the maps' configured vote_weight.
//
// The manager is transport-agnostic: GameServer is responsible for telling
// clients which options are available and for forwarding their picks via
// CastVote(). When the vote concludes, ResolveWinner() yields the next map.
class MapVoteManager {
public:
    struct Candidate {
        std::string mapName;
        std::string displayName;
        int         voteWeight = 50;
        int         votes      = 0;
    };

    explicit MapVoteManager(std::shared_ptr<MapConfig> mapConfig);

    // Configuration (typically sourced from server.ini).
    void SetOptionCount(int n)        { m_optionCount = (n < 2 ? 2 : n); }
    void SetVoteDurationSeconds(int s){ m_voteDurationSeconds = (s < 1 ? 1 : s); }
    void SetEnabled(bool e)           { m_enabled = e; }
    bool IsEnabled() const            { return m_enabled; }
    int  GetVoteDurationSeconds() const { return m_voteDurationSeconds; }

    // Begin a vote. Picks up to GetOptionCount() weighted-random candidates from
    // the available maps, excluding currentMap. Returns the chosen candidates.
    const std::vector<Candidate>& StartVote(const std::string& currentMap);

    // Record a player's vote. optionIndex is into the candidate list returned by
    // StartVote(). A player may change their vote; only their latest pick counts.
    bool CastVote(uint32_t clientId, int optionIndex);

    bool IsVoteActive() const { return m_active; }
    const std::vector<Candidate>& GetCandidates() const { return m_candidates; }

    // Conclude the vote and return the winning map name. Ties are broken by the
    // higher configured vote_weight, then by candidate order. Clears vote state.
    // Returns empty string if no vote was active / no candidates.
    std::string ResolveWinner();

    // Abort an in-progress vote without selecting a winner.
    void Cancel();

private:
    int GetOptionCount() const { return m_optionCount; }

    std::shared_ptr<MapConfig>      m_mapConfig;
    std::vector<Candidate>          m_candidates;
    std::map<uint32_t, int>         m_ballots;       // clientId -> optionIndex
    bool                            m_active   = false;
    bool                            m_enabled  = true;
    int                             m_optionCount = 4;
    int                             m_voteDurationSeconds = 30;
};
