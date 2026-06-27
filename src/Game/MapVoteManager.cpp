// src/Game/MapVoteManager.cpp – Implementation of end-of-round map voting

#include "Game/MapVoteManager.h"
#include "Config/MapConfig.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <random>

MapVoteManager::MapVoteManager(std::shared_ptr<MapConfig> mapConfig)
    : m_mapConfig(std::move(mapConfig))
{
    Logger::Info("MapVoteManager initialized (options=%d, duration=%ds)",
                 m_optionCount, m_voteDurationSeconds);
}

const std::vector<MapVoteManager::Candidate>& MapVoteManager::StartVote(const std::string& currentMap)
{
    m_candidates.clear();
    m_ballots.clear();
    m_active = false;

    if (!m_mapConfig) {
        Logger::Warn("MapVoteManager: no MapConfig; cannot start vote");
        return m_candidates;
    }

    // Build the weighted pool of eligible maps (everything except the current map).
    std::vector<std::string> pool;
    for (const auto& name : m_mapConfig->GetAvailableMaps()) {
        if (name == currentMap) continue;
        pool.push_back(name);
    }
    if (pool.empty()) {
        Logger::Warn("MapVoteManager: no maps available to vote on (only the current map exists)");
        return m_candidates;
    }

    // Weighted random sampling without replacement: repeatedly pick by vote_weight.
    std::mt19937 rng(std::random_device{}());
    int wanted = std::min<int>(GetOptionCount(), static_cast<int>(pool.size()));
    while (static_cast<int>(m_candidates.size()) < wanted && !pool.empty()) {
        long totalWeight = 0;
        for (const auto& name : pool) {
            const MapDefinition* def = m_mapConfig->GetDefinition(name);
            totalWeight += def ? std::max(1, def->voteWeight) : 1;
        }
        std::uniform_int_distribution<long> dist(1, totalWeight);
        long roll = dist(rng);

        size_t chosenIdx = 0;
        long acc = 0;
        for (size_t i = 0; i < pool.size(); ++i) {
            const MapDefinition* def = m_mapConfig->GetDefinition(pool[i]);
            acc += def ? std::max(1, def->voteWeight) : 1;
            if (roll <= acc) { chosenIdx = i; break; }
        }

        const std::string name = pool[chosenIdx];
        const MapDefinition* def = m_mapConfig->GetDefinition(name);
        Candidate c;
        c.mapName     = name;
        c.displayName = (def && !def->displayName.empty()) ? def->displayName : name;
        c.voteWeight  = def ? def->voteWeight : 50;
        c.votes       = 0;
        m_candidates.push_back(c);

        pool.erase(pool.begin() + chosenIdx);
    }

    m_active = !m_candidates.empty();
    Logger::Info("MapVoteManager: started vote with %zu candidates (current map '%s' excluded)",
                 m_candidates.size(), currentMap.c_str());
    for (size_t i = 0; i < m_candidates.size(); ++i) {
        Logger::Debug("  [%zu] %s (weight=%d)", i, m_candidates[i].displayName.c_str(),
                      m_candidates[i].voteWeight);
    }
    return m_candidates;
}

bool MapVoteManager::CastVote(uint32_t clientId, int optionIndex)
{
    if (!m_active) {
        Logger::Debug("MapVoteManager: CastVote ignored (no active vote)");
        return false;
    }
    if (optionIndex < 0 || optionIndex >= static_cast<int>(m_candidates.size())) {
        Logger::Warn("MapVoteManager: client %u cast invalid option %d", clientId, optionIndex);
        return false;
    }

    // Allow vote changes: decrement the previous pick, if any.
    auto it = m_ballots.find(clientId);
    if (it != m_ballots.end()) {
        int prev = it->second;
        if (prev >= 0 && prev < static_cast<int>(m_candidates.size()) && m_candidates[prev].votes > 0) {
            m_candidates[prev].votes--;
        }
    }

    m_ballots[clientId] = optionIndex;
    m_candidates[optionIndex].votes++;
    Logger::Debug("MapVoteManager: client %u voted for '%s' (now %d votes)",
                  clientId, m_candidates[optionIndex].mapName.c_str(),
                  m_candidates[optionIndex].votes);
    return true;
}

std::string MapVoteManager::ResolveWinner()
{
    if (!m_active || m_candidates.empty()) {
        Logger::Debug("MapVoteManager: ResolveWinner with no active vote");
        Cancel();
        return "";
    }

    const Candidate* best = &m_candidates.front();
    for (const auto& c : m_candidates) {
        if (c.votes > best->votes ||
            (c.votes == best->votes && c.voteWeight > best->voteWeight)) {
            best = &c;
        }
    }

    std::string winner = best->mapName;
    Logger::Info("MapVoteManager: vote resolved — winner '%s' with %d votes (weight=%d)",
                 winner.c_str(), best->votes, best->voteWeight);
    Cancel();
    return winner;
}

void MapVoteManager::Cancel()
{
    m_active = false;
    m_candidates.clear();
    m_ballots.clear();
}
