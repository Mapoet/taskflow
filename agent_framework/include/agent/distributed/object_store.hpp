#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework::distributed {

struct ObjectRef {
    std::string tenant_id;
    std::string digest;
    std::uint64_t size{0};
    std::string media_type;
};
struct ObjectStoreCapabilities { bool list{false}, remove{false}; };
struct ObjectListPage { std::vector<ObjectRef> objects; std::string next_cursor; bool complete{true}; std::string error; };
enum class ObjectRemoveStatus { Removed, NotFound, Conflict, Unsupported, Error };
struct ObjectRemoveResult { ObjectRemoveStatus status{ObjectRemoveStatus::Error}; std::string error; explicit operator bool() const noexcept{return status==ObjectRemoveStatus::Removed||status==ObjectRemoveStatus::NotFound;} };

class ObjectStore {
public:
    virtual ~ObjectStore() = default;
    virtual std::optional<ObjectRef> put(std::string_view tenant_id,
                                        std::string_view bytes,
                                        std::string_view media_type,
                                        std::string_view expected_digest = {},
                                        std::string* error = nullptr) = 0;
    virtual std::optional<std::string> get(const ObjectRef& object,
                                           std::string* error = nullptr) const = 0;
    virtual ObjectStoreCapabilities capabilities() const noexcept { return {}; }
    virtual ObjectListPage list(std::string_view, std::string_view = {}, std::size_t = 100) const { return {{},{},true,"object listing unsupported"}; }
    virtual ObjectRemoveResult remove(const ObjectRef&) { return {ObjectRemoveStatus::Unsupported,"object removal unsupported"}; }
};

class FilesystemObjectStore final : public ObjectStore {
public:
    explicit FilesystemObjectStore(std::filesystem::path root,
                                   std::size_t maximum_object_bytes = 64U * 1024U * 1024U);
    std::optional<ObjectRef> put(std::string_view tenant_id, std::string_view bytes,
                                std::string_view media_type,
                                std::string_view expected_digest = {},
                                std::string* error = nullptr) override;
    std::optional<std::string> get(const ObjectRef& object,
                                   std::string* error = nullptr) const override;
    ObjectStoreCapabilities capabilities() const noexcept override { return {true,true}; }
    ObjectListPage list(std::string_view tenant_id, std::string_view cursor = {}, std::size_t limit = 100) const override;
    ObjectRemoveResult remove(const ObjectRef&) override;
private:
    std::optional<std::filesystem::path> path_for(std::string_view tenant_id,
                                                  std::string_view digest) const;
    std::filesystem::path root_;
    std::size_t maximum_object_bytes_;
};

}  // namespace agent_framework::distributed
