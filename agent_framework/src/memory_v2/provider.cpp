#include "agent/memory_v2/provider.hpp"

#include <stdexcept>
#include <utility>

namespace agent_framework::memory_v2 {

StoreMemoryProvider::StoreMemoryProvider(std::string provider_id,
                                         std::shared_ptr<MemoryStore> store)
    : id_(std::move(provider_id)), store_(std::move(store)) {
    if(id_.empty() || !store_) throw std::invalid_argument("provider id and store are required");
}
std::string StoreMemoryProvider::id() const { return id_; }
ProviderResult StoreMemoryProvider::fetch(const MemoryQuery& query) {
    try { return {id_, store_->generation(), store_->query(query), {}}; }
    catch(const std::exception& error) { return {id_, 0, {}, error.what()}; }
}
bool MemoryProviderRegistry::register_provider(std::shared_ptr<MemoryProvider> provider) {
    if(!provider || provider->id().empty()) return false;
    std::lock_guard lock(mutex_);
    return providers_.emplace(provider->id(), std::move(provider)).second;
}
bool MemoryProviderRegistry::remove_provider(const std::string& provider_id) {
    std::lock_guard lock(mutex_);
    return providers_.erase(provider_id) == 1;
}
std::vector<ProviderResult> MemoryProviderRegistry::fetch_all(const MemoryQuery& query) {
    std::vector<std::shared_ptr<MemoryProvider>> providers;
    {
        std::lock_guard lock(mutex_);
        for(const auto& [id, provider] : providers_) { (void)id; providers.push_back(provider); }
    }
    std::vector<ProviderResult> result;
    for(const auto& provider : providers) result.push_back(provider->fetch(query));
    return result;
}
std::vector<std::string> MemoryProviderRegistry::provider_ids() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for(const auto& [id, provider] : providers_) { (void)provider; result.push_back(id); }
    return result;
}
}  // namespace agent_framework::memory_v2
