#include "agent/sandbox/process_provider.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <poll.h>
#include <set>
#include <sstream>
#include <thread>

#include "agent/contracts/contract.hpp"
#include "agent/sandbox/workspace.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent_framework::sandbox {
namespace {
struct Mount { std::filesystem::path host; std::string guest; };
std::optional<Mount> mount(std::string_view value, std::string* error) {
    const auto split = value.rfind(':');
    if (split == std::string_view::npos) { if(error) *error="mount must be host:guest"; return std::nullopt; }
    std::error_code ec; auto host=std::filesystem::weakly_canonical(value.substr(0,split),ec);
    const std::filesystem::path guest(value.substr(split+1));
    bool traversal=false; for(const auto& part:guest) traversal=traversal||part=="..";
    if(ec || !std::filesystem::exists(host) || !guest.is_absolute() || traversal || guest=="/") {
        if(error) *error="unsafe mount";
        return std::nullopt;
    }
    return Mount{std::move(host),guest.generic_string()};
}
std::string stamp() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
#if !defined(_WIN32)
bool write_all(int fd,std::string_view value) {
    while(!value.empty()) { const auto n=::write(fd,value.data(),value.size()); if(n<=0)return false; value.remove_prefix(n); }
    return true;
}
void drain(int fd,std::string& text,std::size_t cap) {
    char data[4096]; for(;;){const auto n=::read(fd,data,sizeof(data));if(n<=0)break;
        if(text.size()<cap) text.append(data,std::min<std::size_t>(cap-text.size(),n));}
}
#endif
}

BubblewrapSandboxProvider::BubblewrapSandboxProvider(
    ProcessSandboxOptions options, std::shared_ptr<CredentialBroker> credentials)
    : options_(std::move(options)), credentials_(std::move(credentials)) {}

bool BubblewrapSandboxProvider::available(std::string* reason) const {
#if defined(_WIN32)
    if(reason) *reason="bubblewrap provider is Linux-only";
    return false;
#else
    if(!std::filesystem::is_regular_file(options_.unshare_path) ||
       !std::filesystem::is_regular_file(options_.bubblewrap_path)) {
        if(reason) *reason="unshare/bwrap executables unavailable";
        return false;
    }
    return true;
#endif
}

std::optional<SandboxHandle> BubblewrapSandboxProvider::create(
    const SandboxSpec& spec,std::string* error) {
    if(spec.provider!="bubblewrap") {if(error)*error="provider mismatch";return std::nullopt;}
    if(const auto issues=validate(spec);!issues.empty()){if(error)*error=issues.front().code;return std::nullopt;}
    if(!spec.network_allowlist.empty()) {
        if(error)*error="network allowlist requires an enforcing proxy; bubblewrap-v1 is deny-all";
        return std::nullopt;
    }
    for(const auto& value:spec.read_only_mounts) if(!mount(value,error)) return std::nullopt;
    for(const auto& value:spec.writable_mounts) if(!mount(value,error)) return std::nullopt;
    if(!spec.credential_refs.empty()&&!credentials_){if(error)*error="credential broker unavailable";return std::nullopt;}
    const auto digest=contracts::canonical_digest(encode(spec)).value_or("");
    if(digest.empty()){if(error)*error="spec digest unavailable";return std::nullopt;}
    const auto id="bwrap-"+digest.substr(digest.size()>16?digest.size()-16:0);
    std::lock_guard lock(mutex_); specs_[id]=spec; return SandboxHandle{id,digest};
}

std::optional<ExecResult> BubblewrapSandboxProvider::exec(
    const SandboxHandle& handle,std::string* error) {
#if defined(_WIN32)
    if(error)*error="unsupported platform"; return std::nullopt;
#else
    SandboxSpec spec; {std::lock_guard lock(mutex_);auto it=specs_.find(handle.sandbox_id);
        if(it==specs_.end()||contracts::canonical_digest(encode(it->second)).value_or("")!=handle.spec_digest){
            if(error)*error="unknown or stale sandbox handle";
            return std::nullopt;
        }
        spec=it->second;
    }
    std::vector<CredentialLease> leases;
    if(credentials_){auto value=credentials_->resolve(spec.credential_refs,error);if(!value)return std::nullopt;leases=std::move(*value);}
    struct SecretPipe{int read{-1};int write{-1};CredentialLease lease;}; std::vector<SecretPipe> secrets;
    for(auto& lease:leases){int fds[2];if(::pipe(fds)!=0){if(error)*error="secret pipe failed";return std::nullopt;}
        secrets.push_back({fds[0],fds[1],lease});}
    int out[2],err[2];if(::pipe(out)||::pipe(err)){if(error)*error="output pipe failed";return std::nullopt;}
    std::vector<std::string> args{options_.unshare_path.string(),"--user","--map-root-user","--net",
        options_.bubblewrap_path.string(),"--die-with-parent","--new-session","--unshare-pid","--unshare-ipc",
        "--unshare-uts","--ro-bind","/usr","/usr","--ro-bind","/bin","/bin","--ro-bind","/lib","/lib",
        "--ro-bind-try","/lib64","/lib64","--proc","/proc","--dev","/dev","--tmpfs","/tmp","--dir","/run","--dir","/run/secrets"};
    for(const auto& value:spec.read_only_mounts){auto m=mount(value,error);args.insert(args.end(),{"--ro-bind",m->host.string(),m->guest});}
    for(const auto& value:spec.writable_mounts){auto m=mount(value,error);args.insert(args.end(),{"--bind",m->host.string(),m->guest});}
    for(std::size_t i=0;i<secrets.size();++i) args.insert(args.end(),{"--file",std::to_string(secrets[i].read),"/run/secrets/credential-"+std::to_string(i)});
    args.push_back("--"); args.insert(args.end(),spec.command.begin(),spec.command.end());
    const auto started=std::chrono::steady_clock::now(); const auto started_at=stamp();
    const pid_t pid=::fork();if(pid<0){if(error)*error="fork failed";return std::nullopt;}
    if(pid==0){(void)::setpgid(0,0);::dup2(out[1],STDOUT_FILENO);::dup2(err[1],STDERR_FILENO);
        ::close(out[0]);::close(err[0]);::close(out[1]);::close(err[1]);for(auto&s:secrets)::close(s.write);
        if(spec.cpu_millis){rlimit r{std::max<std::uint64_t>(1,(spec.cpu_millis+999)/1000),std::max<std::uint64_t>(2,(spec.cpu_millis+999)/1000+1)};(void)::setrlimit(RLIMIT_CPU,&r);}
        if(spec.memory_bytes){rlimit r{spec.memory_bytes,spec.memory_bytes};(void)::setrlimit(RLIMIT_AS,&r);}
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        std::vector<std::string> env{"PATH=/usr/bin:/bin","HOME=/tmp","LANG=C"};std::vector<char*> envp;for(auto& e:env)envp.push_back(e.data());envp.push_back(nullptr);
        ::execve(argv[0],argv.data(),envp.data());_exit(127);}
    (void)::setpgid(pid,pid);::close(out[1]);::close(err[1]);for(auto&s:secrets){::close(s.read);(void)write_all(s.write,s.lease.value);::close(s.write);}
    {std::lock_guard lock(mutex_);process_groups_[handle.sandbox_id]=pid;}
    ::fcntl(out[0],F_SETFL,O_NONBLOCK);::fcntl(err[0],F_SETFL,O_NONBLOCK);
    std::string stdout_text,stderr_text;int status=0;bool timed_out=false;struct rusage usage{};
    const auto deadline=started+std::chrono::milliseconds(spec.wall_time_ms);
    for(;;){pollfd fds[2]{{out[0],POLLIN,0},{err[0],POLLIN,0}};(void)::poll(fds,2,25);drain(out[0],stdout_text,options_.output_limit_bytes);drain(err[0],stderr_text,options_.output_limit_bytes);
        const auto waited=::wait4(pid,&status,WNOHANG,&usage);if(waited==pid)break;if(waited<0){if(error)*error="wait4 failed";return std::nullopt;}
        if(std::chrono::steady_clock::now()>=deadline){timed_out=true;(void)::kill(-pid,SIGKILL);(void)::kill(pid,SIGKILL);(void)::wait4(pid,&status,0,&usage);break;}}
    drain(out[0],stdout_text,options_.output_limit_bytes);drain(err[0],stderr_text,options_.output_limit_bytes);::close(out[0]);::close(err[0]);
    {std::lock_guard lock(mutex_);process_groups_.erase(handle.sandbox_id);}
    CredentialBroker::redact(stdout_text,leases);CredentialBroker::redact(stderr_text,leases);
    ExecResult result;result.timed_out=timed_out;result.exit_code=timed_out?-1:WIFEXITED(status)?WEXITSTATUS(status):-1;result.stdout_text=stdout_text;result.stderr_text=stderr_text;
    result.manifest.metadata=spec.metadata;result.manifest.sandbox_id=handle.sandbox_id;result.manifest.spec_digest=handle.spec_digest;result.manifest.provider_version=version();
    result.manifest.workspace_input_digest=spec.workspace_base_digest;result.manifest.stdout_digest=contracts::embedded_digest(stdout_text).value_or("");result.manifest.stderr_digest=contracts::embedded_digest(stderr_text).value_or("");
    result.manifest.exit_code=result.exit_code;result.manifest.cpu_millis=(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000ULL+(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)/1000ULL;
    result.manifest.peak_memory_bytes=static_cast<std::uint64_t>(usage.ru_maxrss)*1024ULL;result.manifest.wall_time_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();result.manifest.started_at=started_at;result.manifest.finished_at=stamp();
    if(!spec.writable_mounts.empty()){auto m=mount(spec.writable_mounts.front(),error);auto snapshot=snapshot_workspace(m->host,options_.workspace_quota_bytes,error);if(!snapshot)return std::nullopt;result.manifest.workspace_output_digest=snapshot->digest;}else result.manifest.workspace_output_digest=spec.workspace_base_digest;
    return result;
#endif
}

bool BubblewrapSandboxProvider::destroy(const SandboxHandle& handle,std::string* error) {
    std::lock_guard lock(mutex_);if(specs_.erase(handle.sandbox_id)==0){if(error)*error="unknown sandbox handle";return false;}return true;
}
SandboxCancelResult BubblewrapSandboxProvider::cancel(const SandboxHandle&handle,SandboxSignal signal){
#if defined(_WIN32)
    return {false,false,false,"unsupported platform",{}};
#else
    long pid=0;{std::lock_guard lock(mutex_);auto it=process_groups_.find(handle.sandbox_id);if(it==process_groups_.end())return {false,false,false,"sandbox process not running",{}};pid=it->second;}
    const int value=signal==SandboxSignal::Kill?SIGKILL:SIGTERM;const bool accepted=::kill(-static_cast<pid_t>(pid),value)==0||::kill(static_cast<pid_t>(pid),value)==0;
    return {accepted,false,false,accepted?(signal==SandboxSignal::Kill?"process_group_sigkill_sent":"process_group_sigterm_sent"):"process_group_signal_failed",{}};
#endif
}
}  // namespace agent_framework::sandbox
