#include "agent/distributed/object_store.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/evp.h>
#endif

namespace agent_framework::distributed
{
    namespace
    {
        void fail(std::string *error, std::string message)
        {
            if (error)
                *error = std::move(message);
        }
        bool safe_component(std::string_view value)
        {
            if (value.empty() || value.size() > 128)
                return false;
            for (const unsigned char c : value)
                if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.'))
                    return false;
            return value != "." && value != "..";
        }
        std::optional<std::string> sha256(std::string_view bytes, std::string *error)
        {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
            EVP_MD_CTX *context = EVP_MD_CTX_new();
            if (!context)
            {
                fail(error, "sha256 allocation failed");
                return std::nullopt;
            }
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int size = 0;
            const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                            EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
                            EVP_DigestFinal_ex(context, digest.data(), &size) == 1;
            EVP_MD_CTX_free(context);
            if (!ok)
            {
                fail(error, "sha256 failed");
                return std::nullopt;
            }
            std::ostringstream out;
            out << "sha256:" << std::hex << std::setfill('0');
            for (unsigned int i = 0; i < size; ++i)
                out << std::setw(2) << static_cast<unsigned int>(digest[i]);
            return out.str();
#else
            (void)bytes;
            fail(error, "sha256 unavailable");
            return std::nullopt;
#endif
        }
        bool write_all(int fd, std::string_view bytes)
        {
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                offset += static_cast<std::size_t>(count);
            }
            return true;
        }
    }
    FilesystemObjectStore::FilesystemObjectStore(std::filesystem::path root, std::size_t maximum)
        : root_(std::move(root)), maximum_object_bytes_(maximum)
    {
        if (root_.empty() || maximum == 0)
            throw std::invalid_argument("object store root and limit required");
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec)
            throw std::runtime_error(ec.message());
    }
    std::optional<std::filesystem::path> FilesystemObjectStore::path_for(std::string_view tenant, std::string_view digest) const
    {
        if (!safe_component(tenant) || digest.size() != 71 || digest.rfind("sha256:", 0) != 0)
            return std::nullopt;
        for (std::size_t i = 7; i < digest.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(digest[i])) || std::isupper(static_cast<unsigned char>(digest[i])))
                return std::nullopt;
        return root_ / std::string(tenant) / std::string(digest.substr(7, 2)) / std::string(digest.substr(7));
    }
    std::optional<ObjectRef> FilesystemObjectStore::put(std::string_view tenant, std::string_view bytes, std::string_view media, std::string_view expected, std::string *error)
    {
        if (bytes.size() > maximum_object_bytes_)
        {
            fail(error, "object size limit exceeded");
            return std::nullopt;
        }
        auto digest = sha256(bytes, error);
        if (!digest)
            return std::nullopt;
        if (!expected.empty() && expected != *digest)
        {
            fail(error, "object digest mismatch");
            return std::nullopt;
        }
        auto target = path_for(tenant, *digest);
        if (!target)
        {
            fail(error, "invalid tenant or digest");
            return std::nullopt;
        }
        std::error_code ec;
        std::filesystem::create_directories(target->parent_path(), ec);
        if (ec)
        {
            fail(error, ec.message());
            return std::nullopt;
        }
        if (std::filesystem::exists(*target))
        {
            ObjectRef ref{std::string(tenant), *digest, bytes.size(), std::string(media)};
            auto existing = get(ref, error);
            if (!existing || *existing != bytes)
            {
                fail(error, "existing content-addressed object corrupted");
                return std::nullopt;
            }
            return ref;
        }
        const auto temporary = target->string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
        {
            fail(error, std::strerror(errno));
            return std::nullopt;
        }
        const bool written = write_all(fd, bytes) && ::fsync(fd) == 0;
        const int close_status = ::close(fd);
        if (!written || close_status != 0)
        {
            ::unlink(temporary.c_str());
            fail(error, "durable object write failed");
            return std::nullopt;
        }
        if (::rename(temporary.c_str(), target->c_str()) != 0)
        {
            ::unlink(temporary.c_str());
            fail(error, std::strerror(errno));
            return std::nullopt;
        }
        const int directory = ::open(target->parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0)
        {
            ::fsync(directory);
            ::close(directory);
        }
        return ObjectRef{std::string(tenant), *digest, bytes.size(), std::string(media)};
    }
    std::optional<std::string> FilesystemObjectStore::get(const ObjectRef &ref, std::string *error) const
    {
        auto path = path_for(ref.tenant_id, ref.digest);
        if (!path)
        {
            fail(error, "invalid object reference");
            return std::nullopt;
        }
        std::ifstream input(*path, std::ios::binary);
        if (!input)
        {
            fail(error, "object not found");
            return std::nullopt;
        }
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.size() > maximum_object_bytes_ || bytes.size() != ref.size)
        {
            fail(error, "object size verification failed");
            return std::nullopt;
        }
        auto digest = sha256(bytes, error);
        if (!digest || *digest != ref.digest)
        {
            fail(error, "object integrity verification failed");
            return std::nullopt;
        }
        return bytes;
    }
    ObjectListPage FilesystemObjectStore::list(std::string_view tenant,std::string_view cursor,std::size_t limit) const
    {
        ObjectListPage out;if(!safe_component(tenant)||limit==0||limit>1000){out.error="invalid tenant or list limit";return out;}
        std::vector<std::string> digests;std::error_code ec;const auto root=root_/std::string(tenant);if(!std::filesystem::exists(root,ec))return out;
        for(std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec)){
            const auto status=it->symlink_status(ec);if(ec)break;if(!std::filesystem::is_regular_file(status)||std::filesystem::is_symlink(status))continue;const auto name=it->path().filename().string();if(name.size()!=64)continue;auto digest=std::string("sha256:")+name;if(path_for(tenant,digest)==it->path())digests.push_back(std::move(digest));
        }
        if(ec){out.error=ec.message();return out;}std::sort(digests.begin(),digests.end());auto begin=cursor.empty()?digests.begin():std::upper_bound(digests.begin(),digests.end(),std::string(cursor));
        for(;begin!=digests.end()&&out.objects.size()<limit;++begin){auto path=path_for(tenant,*begin);auto size=std::filesystem::file_size(*path,ec);if(ec){out.error=ec.message();return out;}out.objects.push_back({std::string(tenant),*begin,size,"application/octet-stream"});}
        out.complete=begin==digests.end();if(!out.complete&&!out.objects.empty())out.next_cursor=out.objects.back().digest;return out;
    }
    ObjectRemoveResult FilesystemObjectStore::remove(const ObjectRef& ref)
    {
        auto path=path_for(ref.tenant_id,ref.digest);if(!path)return {ObjectRemoveStatus::Conflict,"invalid object reference"};std::error_code ec;auto status=std::filesystem::symlink_status(*path,ec);if(ec==std::errc::no_such_file_or_directory||!std::filesystem::exists(status))return {ObjectRemoveStatus::NotFound,{}};if(ec)return {ObjectRemoveStatus::Error,ec.message()};if(!std::filesystem::is_regular_file(status)||std::filesystem::is_symlink(status))return {ObjectRemoveStatus::Conflict,"object path is not a regular file"};
        std::string error;if(!get(ref,&error))return {ObjectRemoveStatus::Conflict,error};if(!std::filesystem::remove(*path,ec)||ec)return {ObjectRemoveStatus::Error,ec?ec.message():"object removal failed"};const int directory=::open(path->parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory>=0){::fsync(directory);::close(directory);}return {ObjectRemoveStatus::Removed,{}};
    }
} // namespace agent_framework::distributed
