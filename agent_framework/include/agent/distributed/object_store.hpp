#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace agent_framework::distributed {

struct ObjectRef {
    std::string tenant_id;
    std::string digest;
    std::uint64_t size{0};
    std::string media_type;
};

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
private:
    std::optional<std::filesystem::path> path_for(std::string_view tenant_id,
                                                  std::string_view digest) const;
    std::filesystem::path root_;
    std::size_t maximum_object_bytes_;
};

}  // namespace agent_framework::distributed
