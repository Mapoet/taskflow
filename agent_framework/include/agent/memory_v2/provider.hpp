#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/memory_v2/store.hpp"

namespace agent_framework::memory_v2 {

struct ProviderResult {
    std::string provider_id;
    std::uint64_t generation{0};
    std::vector<MemoryRecord> records;
    std::string error;
};

class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
    virtual std::string id() const = 0;
    virtual ProviderResult fetch(const MemoryQuery& query) = 0;
};

class StoreMemoryProvider final : public MemoryProvider {
public:
    StoreMemoryProvider(std::string provider_id, std::shared_ptr<MemoryStore> store);
    std::string id() const override;
    ProviderResult fetch(const MemoryQuery& query) override;
private:
    std::string id_;
    std::shared_ptr<MemoryStore> store_;
};

class MemoryProviderRegistry {
public:
    bool register_provider(std::shared_ptr<MemoryProvider> provider);
    bool remove_provider(const std::string& provider_id);
    std::vector<ProviderResult> fetch_all(const MemoryQuery& query);
    std::vector<std::string> provider_ids() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<MemoryProvider>> providers_;
};

}  // namespace agent_framework::memory_v2
