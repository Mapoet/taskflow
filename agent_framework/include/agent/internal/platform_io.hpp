#pragma once

#include <cstdint>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace agent_framework::internal {

inline std::int64_t current_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

#if !defined(_WIN32)
// fsync is available on both Linux and macOS and provides the durability
// semantics required by journals that may have created or extended a file.
inline int sync_file(int file_descriptor) noexcept {
    return ::fsync(file_descriptor);
}
#endif

}  // namespace agent_framework::internal
