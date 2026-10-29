#pragma once

#include "json.hpp"
#include <cstdint>
#include <memory>
#include <set>
#include <string>

class ChatArchive {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    explicit ChatArchive(const std::string& directory);
    ~ChatArchive();
    nlohmann::json recent(size_t limit);
    uint64_t lastSequence();
    nlohmann::json find(const std::string& group, const std::string& id, uint64_t before, int64_t timestamp);
    void append(const nlohmann::json& records);
    void maintain();
    nlohmann::json retrieve(const std::string& group, uint64_t before, int64_t timestamp,
                            const std::string& query, const std::set<uint64_t>& excluded);
    void shutdown();
};
