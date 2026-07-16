/**
 * @file skill_script_tool.cpp
 * @brief Policy-bound Script/CLI execution in a fail-closed Linux sandbox.
 */

#include <agent/skills/skill_script_tool.hpp>
#include <agent/skills/skill_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {

std::size_t output_cap() {
    const char* raw = std::getenv("AGENT_SKILL_SCRIPT_OUTPUT_MAX_BYTES");
    if (!raw || !*raw) return 65536;
    const long long value = std::atoll(raw);
    return value > 0 ? static_cast<std::size_t>(value) : 65536;
}

int process_timeout_sec() {
    const char* raw = std::getenv("AGENT_SKILL_SCRIPT_TIMEOUT_SEC");
    if (!raw || !*raw) return 30;
    const int value = std::atoi(raw);
    return value > 0 ? value : 30;
}

std::vector<std::string> interpreter_allowlist() {
    std::vector<std::string> out;
    const char* raw = std::getenv("AGENT_SKILL_SCRIPT_ALLOWLIST");
    if (!raw || !*raw) return out;
    std::string item;
    for (const char* cursor = raw;; ++cursor) {
        if (*cursor == ',' || *cursor == '\0') {
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
                item.erase(item.begin());
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
                item.pop_back();
            if (!item.empty()) out.push_back(std::move(item));
            item.clear();
            if (*cursor == '\0') break;
        } else {
            item.push_back(*cursor);
        }
    }
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string interpreter_for(const std::filesystem::path& path,
                            const std::vector<std::string>& allowed,
                            SkillResourceType kind) {
    const std::string extension = path.extension().string();
    if (extension == ".sh" && contains(allowed, "/bin/sh")) return "/bin/sh";
    if (extension == ".py" && contains(allowed, "/usr/bin/python3")) return "/usr/bin/python3";
    if (extension == ".py" && contains(allowed, "/bin/python3")) return "/bin/python3";
    if (kind == SkillResourceType::Cli && extension.empty()) return {};
    return {};
}

json tool_error(const std::string& code, const std::string& message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

std::string secret_name(std::string value) {
    for (char& c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        else c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

void redact(std::string& text, const std::vector<std::string>& secrets) {
    for (const auto& secret : secrets) {
        if (secret.empty()) continue;
        std::size_t position = 0;
        while ((position = text.find(secret, position)) != std::string::npos) {
            text.replace(position, secret.size(), "[REDACTED]");
            position += 10;
        }
    }
}

std::optional<SkillResourceDescriptor> find_resource(const SkillManifest& manifest,
                                                     const std::string& path,
                                                     SkillResourceType kind) {
    const auto found = std::find_if(manifest.resources.begin(), manifest.resources.end(),
                                    [&](const SkillResourceDescriptor& resource) {
        return resource.kind == kind && resource.path == path;
    });
    if (found == manifest.resources.end()) return std::nullopt;
    return *found;
}

SkillInvocationContext invocation_context(const ToolCallControl& control,
                                          const SkillManifest& manifest) {
    if (control.skill_context) return *control.skill_context;
    SkillInvocationContext context;
    if (manifest.legacy_v0) {
        context.grants.tools = manifest.permissions.tools;
        context.grants.network = manifest.permissions.network;
        context.grants.environment = manifest.permissions.environment;
        context.grants.filesystem_read = manifest.permissions.filesystem_read;
        context.grants.filesystem_write = manifest.permissions.filesystem_write;
        context.grants.secrets = manifest.permissions.secrets;
    }
    return context;
}

#if !defined(_WIN32)
bool write_all(int fd, const std::string& value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t written = ::write(fd, value.data() + offset, value.size() - offset);
        if (written < 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

json run_sandboxed_process(const SkillInvocationTicket& ticket,
                           const std::filesystem::path& package,
                           const std::vector<std::string>& arguments,
                           const json& input,
                           const ToolCallControl& control) {
    namespace fs = std::filesystem;
    if (!fs::is_regular_file("/usr/bin/unshare") || !fs::is_regular_file("/usr/bin/bwrap")) {
        return SkillRuntime::permission_error(
            {false, SkillPermissionKind::FilesystemRead, "sandbox", ticket.resource.path,
             "required unshare/bwrap sandbox backend is unavailable"});
    }
    const auto allowed_interpreters = interpreter_allowlist();
    const fs::path host_target = fs::weakly_canonical(package / ticket.resource.path);
    std::string interpreter = interpreter_for(host_target, allowed_interpreters,
                                              ticket.resource.kind);
    if (ticket.resource.kind == SkillResourceType::Script && interpreter.empty()) {
        return tool_error("validation_failed", "no allowlisted interpreter for script type");
    }
    const std::string sandbox_target = "/skill/" + ticket.resource.path;

    struct SecretPipe {
        int read_fd = -1;
        int write_fd = -1;
        std::string reference;
        std::string value;
    };
    std::vector<SecretPipe> secret_pipes;
    if (ticket.context.secret_provider) {
        for (const auto& reference : ticket.manifest->permissions.secrets) {
            if (!ticket.policy->authorize_secret(reference).allowed) continue;
            auto value = ticket.context.secret_provider(reference);
            if (!value) continue;
            if (value->size() > 65536U) {
                return tool_error(kSkillResourceBudgetExceeded, "secret exceeds 65536 bytes");
            }
            int descriptors[2];
            if (::pipe(descriptors) != 0) return tool_error("tool_internal_error", "secret pipe failed");
            secret_pipes.push_back({descriptors[0], descriptors[1], reference, std::move(*value)});
        }
    }

    int input_pipe[2];
    int output_pipe[2];
    int error_pipe[2];
    if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0) {
        return tool_error("tool_internal_error", "process pipe failed");
    }

    std::vector<std::string> command{
        "/usr/bin/unshare", "--user", "--map-root-user", "--net", "/usr/bin/bwrap",
        "--die-with-parent", "--new-session", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
        "--ro-bind", "/usr", "/usr", "--ro-bind", "/bin", "/bin",
        "--ro-bind", "/lib", "/lib", "--ro-bind-try", "/lib64", "/lib64",
        "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
        "--dir", "/run", "--dir", "/run/secrets",
        "--ro-bind", package.string(), "/skill", "--chdir", "/skill"};

    for (const auto& scope : ticket.manifest->permissions.filesystem_write) {
        const fs::path relative(scope);
        if (scope == "*" || relative.is_absolute()) continue;
        bool unsafe = false;
        for (const auto& part : relative) unsafe = unsafe || part == "..";
        if (unsafe) continue;
        const fs::path host = fs::weakly_canonical(package / relative);
        if (!fs::is_directory(host) || !ticket.policy->authorize_filesystem(host, true).allowed) continue;
        command.insert(command.end(), {"--bind", host.string(), "/skill/" + relative.string()});
    }
    for (const auto& secret : secret_pipes) {
        command.insert(command.end(), {"--file", std::to_string(secret.read_fd),
                                       "/run/secrets/" + secret_name(secret.reference)});
    }
    if (interpreter.empty()) command.push_back(sandbox_target);
    else {
        command.push_back(interpreter);
        command.push_back(sandbox_target);
    }
    command.insert(command.end(), arguments.begin(), arguments.end());

    std::vector<std::string> environment{"PATH=/usr/bin:/bin", "LANG=C", "HOME=/tmp"};
    for (const auto& name : ticket.manifest->permissions.environment) {
        if (!ticket.policy->authorize_environment(name).allowed) continue;
        const auto found = ticket.context.environment.find(name);
        if (found != ticket.context.environment.end()) environment.push_back(name + "=" + found->second);
    }
    for (const auto& secret : secret_pipes) {
        environment.push_back("AGENT_SECRET_" + secret_name(secret.reference) +
                              "_FILE=/run/secrets/" + secret_name(secret.reference));
    }

    const pid_t pid = ::fork();
    if (pid < 0) return tool_error("tool_internal_error", "fork failed");
    if (pid == 0) {
        (void)::setpgid(0, 0);
        if(ticket.context.limits.max_cpu_time > std::chrono::milliseconds::zero()) {
            const auto milliseconds = ticket.context.limits.max_cpu_time.count();
            const rlim_t seconds = static_cast<rlim_t>(std::max<std::int64_t>(
                1, (milliseconds + 999) / 1000));
            const struct rlimit limit{seconds, seconds + 1};
            if(::setrlimit(RLIMIT_CPU, &limit) != 0) _exit(126);
        }
        if(ticket.context.limits.max_memory_bytes > 0) {
            const auto bytes = static_cast<rlim_t>(ticket.context.limits.max_memory_bytes);
            const struct rlimit limit{bytes, bytes};
            if(::setrlimit(RLIMIT_AS, &limit) != 0) _exit(126);
        }
        ::close(input_pipe[1]);
        ::close(output_pipe[0]);
        ::close(error_pipe[0]);
        ::dup2(input_pipe[0], STDIN_FILENO);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(error_pipe[1], STDERR_FILENO);
        ::close(input_pipe[0]);
        ::close(output_pipe[1]);
        ::close(error_pipe[1]);
        for (const auto& secret : secret_pipes) ::close(secret.write_fd);
        std::vector<char*> argv;
        for (auto& value : command) argv.push_back(value.data());
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (auto& value : environment) envp.push_back(value.data());
        envp.push_back(nullptr);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    (void)::setpgid(pid, pid);
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    for (auto& secret : secret_pipes) {
        ::close(secret.read_fd);
        (void)write_all(secret.write_fd, secret.value);
        ::close(secret.write_fd);
    }
    const std::string serialized_input = input.dump();
    (void)write_all(input_pipe[1], serialized_input);
    ::close(input_pipe[1]);
    ::fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(error_pipe[0], F_SETFL, O_NONBLOCK);

    std::string stdout_text;
    std::string stderr_text;
    const std::size_t cap = std::min(output_cap(), ticket.context.limits.max_output_bytes);
    auto drain = [&](int fd, std::string& output) {
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count <= 0) break;
            if (output.size() < cap) {
                const std::size_t room = cap - output.size();
                output.append(buffer, std::min(room, static_cast<std::size_t>(count)));
            }
        }
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(process_timeout_sec());
    int status = 0;
    bool cancelled = false;
    bool timed_out = false;
    bool done = false;
    while (!done) {
        struct pollfd descriptors[2]{{output_pipe[0], POLLIN, 0}, {error_pipe[0], POLLIN, 0}};
        (void)::poll(descriptors, 2, 50);
        drain(output_pipe[0], stdout_text);
        drain(error_pipe[0], stderr_text);
        const int waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) done = true;
        else if (waited < 0) {
            ::close(output_pipe[0]);
            ::close(error_pipe[0]);
            return tool_error("tool_internal_error", "waitpid failed");
        }
        if (!done && control.should_stop()) cancelled = true;
        if (!done && std::chrono::steady_clock::now() >= deadline) timed_out = true;
        if (cancelled || timed_out) {
            (void)::kill(-pid, SIGKILL);
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            done = true;
        }
    }
    drain(output_pipe[0], stdout_text);
    drain(error_pipe[0], stderr_text);
    ::close(output_pipe[0]);
    ::close(error_pipe[0]);
    std::vector<std::string> secret_values;
    for (const auto& secret : secret_pipes) secret_values.push_back(secret.value);
    redact(stdout_text, secret_values);
    redact(stderr_text, secret_values);
    json result{{"stdout", stdout_text}, {"stderr", stderr_text},
                {"truncated", stdout_text.size() >= cap || stderr_text.size() >= cap}};
    if (cancelled || timed_out) {
        result["exit_code"] = -1;
        result[cancelled ? "cancelled" : "timed_out"] = true;
        result["code"] = kSkillCancelled;
    } else if (WIFSIGNALED(status) &&
               ticket.context.limits.max_cpu_time > std::chrono::milliseconds::zero() &&
               (WTERMSIG(status) == SIGXCPU || WTERMSIG(status) == SIGKILL)) {
        result["exit_code"] = -1;
        result["signaled"] = true;
        result["signal"] = WTERMSIG(status);
        result["budget_exceeded"] = "cpu";
        result["code"] = kSkillResourceBudgetExceeded;
    } else if (WIFEXITED(status)) {
        result["exit_code"] = WEXITSTATUS(status);
        if(ticket.context.limits.max_cpu_time > std::chrono::milliseconds::zero() &&
           (WEXITSTATUS(status) == 128 + SIGXCPU ||
            WEXITSTATUS(status) == 128 + SIGKILL)) {
            result["code"] = kSkillResourceBudgetExceeded;
            result["budget_exceeded"] = "cpu";
        } else if(WEXITSTATUS(status) == 126 &&
           (ticket.context.limits.max_cpu_time > std::chrono::milliseconds::zero() ||
            ticket.context.limits.max_memory_bytes > 0)) {
            result["code"] = kSkillResourceBudgetExceeded;
            result["budget_exceeded"] = "limit_setup";
        }
    }
    else {
        result["exit_code"] = -1;
        result["signaled"] = true;
    }
    return result;
}
#endif

json execute_skill_process(const std::shared_ptr<SkillServices>& services,
                           const json& arguments, const ToolCallControl& control,
                           SkillResourceType kind) {
    const std::string skill_id = arguments.at("skill_id").get<std::string>();
    const std::string relative_path = arguments.at("relative_path").get<std::string>();
    const auto entry = services->registry->get(skill_id);
    const auto manifest = services->registry->get_manifest(skill_id);
    if (!entry || !manifest) return tool_error("validation_failed", "unknown skill_id");
    if (!manifest->legacy_v0 && (!control.skill_context || control.active_skill_id != skill_id)) {
        return json{{"error", "v1 skill execution requires matching request context"},
                    {"code", kSkillPermissionDenied}};
    }
    const auto resource = find_resource(*manifest, relative_path, kind);
    if (!resource) return tool_error("validation_failed", "resource is not declared for requested kind");
    if (!resource->executable) return tool_error("validation_failed", "resource is not executable");
    std::vector<std::string> process_arguments;
    std::size_t argument_bytes = 0;
    if (arguments.contains("args")) {
        for (const auto& value : arguments.at("args")) {
            const std::string item = value.get<std::string>();
            argument_bytes += item.size();
            if (argument_bytes > 4096U) return tool_error("validation_failed", "args exceed 4096 bytes");
            process_arguments.push_back(item);
        }
    }
    const json input = arguments.value("input", json::object());
    auto runtime = services->runtime;
    if (!runtime) runtime = std::make_shared<SkillRuntime>(services->registry, services->loader);
    auto context = invocation_context(control, *manifest);
    auto begun = runtime->begin(skill_id, resource->id, kind, input, std::move(context));
    if (!begun.ok || !begun.ticket) return begun.error;
    const auto package = entry->script_jail.value_or(entry->file_path.parent_path());
    if (!manifest->legacy_v0) {
        const auto read = begun.ticket->policy->authorize_filesystem(package / relative_path, false);
        if (!read.allowed) return SkillRuntime::permission_error(read);
    }
#if defined(_WIN32)
    return tool_error("unsupported", "Skill process sandbox is unavailable on Windows");
#else
    json result = run_sandboxed_process(*begun.ticket, package, process_arguments, input, control);
    if (result.value("code", std::string{}) == kSkillCancelled) {
        runtime->record_termination(*begun.ticket, result.value("timed_out", false));
        return result;
    }
    if (result.value("code", std::string{}) == kSkillResourceBudgetExceeded) {
        runtime->record_budget_exceeded(
            *begun.ticket, result.value("budget_exceeded", std::string("process")));
        return result;
    }
    json contract_output = result;
    if (!resource->output_schema.empty()) {
        try {
            contract_output = json::parse(result.value("stdout", std::string{}));
        } catch (const std::exception&) {
            contract_output = result.value("stdout", std::string{});
        }
    }
    auto finished = runtime->finish(*begun.ticket, contract_output);
    if (!finished.ok) return finished.error;
    if (!resource->output_schema.empty()) result["output"] = std::move(contract_output);
    return result;
#endif
}

SkillResourceKind resource_kind_from_text(const std::string& value) {
    static const std::map<std::string, SkillResourceKind> values = {
        {"script",SkillResourceKind::Script},{"cli",SkillResourceKind::Cli},
        {"reference",SkillResourceKind::Reference},{"tool",SkillResourceKind::Tool},
        {"mcp",SkillResourceKind::Mcp},{"template",SkillResourceKind::Template},
        {"schema",SkillResourceKind::Schema},{"prompt",SkillResourceKind::Prompt},
        {"workflow",SkillResourceKind::Workflow},{"config",SkillResourceKind::Config},
        {"asset",SkillResourceKind::Asset},{"model",SkillResourceKind::Model},
        {"test",SkillResourceKind::Test}};
    const auto found = values.find(value);
    return found == values.end() ? SkillResourceKind::AnyDeclared : found->second;
}

ToolMeta process_meta(const char* name, const char* description) {
    ToolMeta meta;
    meta.name = name;
    meta.description = description;
    meta.schema = json::parse(R"({
      "type":"object",
      "properties":{
        "skill_id":{"type":"string"},
        "relative_path":{"type":"string"},
        "args":{"type":"array","items":{"type":"string"},"maxItems":32},
        "input":{"type":"object","additionalProperties":true}
      },
      "required":["skill_id","relative_path"]
    })");
    meta.side_effect = ToolSideEffect::Write;
    return meta;
}

} // namespace

void register_skill_script_tool(ToolBus& bus, const std::shared_ptr<SkillServices>& services) {
    if (!services || !services->registry || !services->loader) return;
    if (!services->runtime) {
        services->runtime = std::make_shared<SkillRuntime>(services->registry, services->loader);
    }
    if (!bus.get_tool_info("read_skill_resource")) {
        ToolMeta meta;
        meta.name = "read_skill_resource";
        meta.description = "Read a declared typed resource from an indexed Skill package.";
        meta.schema = json::parse(R"({
          "type":"object",
          "properties":{"skill_id":{"type":"string"},"relative_path":{"type":"string"},
                        "kind":{"type":"string","enum":["script","cli","reference","tool","mcp","template","schema","prompt","workflow","config","asset","model","test"]},
                        "max_bytes":{"type":"integer","minimum":1,"maximum":262144}},
          "required":["skill_id","relative_path","kind"]
        })");
        meta.side_effect = ToolSideEffect::ReadOnly;
        bus.register_cancellable_local_tool("read_skill_resource", [services](
            const json& args, const ToolCallControl& control) {
            const std::string skill_id = args.at("skill_id").get<std::string>();
            const std::string relative_path = args.at("relative_path").get<std::string>();
            const auto entry = services->registry->get(skill_id);
            const auto manifest = services->registry->get_manifest(skill_id);
            if (!entry || !manifest) return tool_error("validation_failed", "unknown skill_id");
            if (!manifest->legacy_v0) {
                if (!control.skill_context || control.active_skill_id != skill_id) {
                    return json{{"error", "v1 resource access requires matching request context"},
                                {"code", kSkillPermissionDenied}};
                }
                const auto package = entry->script_jail.value_or(entry->file_path.parent_path());
                SkillPolicyEngine policy(manifest->permissions,
                                         control.skill_context->grants, package);
                const auto read = policy.authorize_filesystem(package / relative_path, false);
                if (!read.allowed) return SkillRuntime::permission_error(read);
            }
            std::string error;
            auto content = services->loader->load_resource(
                skill_id, relative_path,
                resource_kind_from_text(args.at("kind").get<std::string>()),
                args.value("max_bytes", 65536U), &error);
            if (!content) return tool_error("validation_failed", error);
            return json{{"content", *content}, {"bytes", content->size()}, {"truncated", false}};
        }, meta);
    }
    if (!bus.get_tool_info("run_skill_script")) {
        bus.register_cancellable_local_tool(
            "run_skill_script",
            [services](const json& args, const ToolCallControl& control) {
                return execute_skill_process(services, args, control, SkillResourceType::Script);
            },
            process_meta("run_skill_script", "Run a declared Skill script in a networkless sandbox."));
    }
    if (!bus.get_tool_info("run_skill_cli")) {
        bus.register_cancellable_local_tool(
            "run_skill_cli",
            [services](const json& args, const ToolCallControl& control) {
                return execute_skill_process(services, args, control, SkillResourceType::Cli);
            },
            process_meta("run_skill_cli", "Run a declared Skill CLI program in a networkless sandbox."));
    }
}

} // namespace agent_framework
