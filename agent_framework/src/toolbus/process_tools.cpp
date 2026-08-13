#include <agent/toolbus/process_tools.hpp>

#include <agent/sandbox/process_provider.hpp>
#include <agent/sandbox/workspace.hpp>
#include <agent/toolbus/fs_sandbox.hpp>
#include <agent/toolbus/web_tools.hpp>
#include <agent/toolbus/web_http.hpp>
#include <agent/contracts/contract.hpp>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <fstream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/evp.h>
#endif

namespace agent_framework {
namespace fs = std::filesystem;
namespace {
std::mutex credential_mutex;
std::shared_ptr<sandbox::CredentialBroker> credential_broker;
std::mutex object_store_mutex;
std::shared_ptr<distributed::ObjectStore> network_object_store;
std::string network_object_tenant{"local"};
json process_error(std::string code, std::string message) {
    return json{{"error",{{"code",std::move(code)},{"message",std::move(message)}}}};
}
std::string base64(std::string_view value) {
    static constexpr char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; unsigned val=0; int bits=-6;
    for(unsigned char c:value){val=(val<<8)+c;bits+=8;while(bits>=0){out.push_back(table[(val>>bits)&63]);bits-=6;}}
    if(bits>-6) out.push_back(table[((val<<8)>>(bits+8))&63]);
    while(out.size()%4) out.push_back('=');
    return out;
}
std::optional<std::string> digest_bytes(std::string_view bytes,std::string_view algorithm) {
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    const EVP_MD* md=algorithm=="sha256"?EVP_sha256():algorithm=="sha512"?EVP_sha512():nullptr;
    if(!md) return std::nullopt;
    EVP_MD_CTX* ctx=EVP_MD_CTX_new();
    if(!ctx)return std::nullopt;
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned size=0;
    const bool ok=EVP_DigestInit_ex(ctx,md,nullptr)==1&&EVP_DigestUpdate(ctx,bytes.data(),bytes.size())==1&&EVP_DigestFinal_ex(ctx,digest,&size)==1;
    EVP_MD_CTX_free(ctx); if(!ok)return std::nullopt; std::ostringstream out;out<<algorithm<<":"<<std::hex<<std::setfill('0');
    for(unsigned i=0;i<size;++i)out<<std::setw(2)<<static_cast<unsigned>(digest[i]);
    return out.str();
#else
    (void)bytes;(void)algorithm;return std::nullopt;
#endif
}
json run_sandboxed(const FsSandboxConfig& cfg, const std::vector<std::string>& command,
                   const json& args) {
    std::string error;
    auto provider = std::make_shared<sandbox::BubblewrapSandboxProvider>(sandbox::ProcessSandboxOptions{});
    if (!provider->available(&error)) return process_error("sandbox_unavailable", error);
    auto snapshot = sandbox::snapshot_workspace(cfg.root, 256U * 1024U * 1024U, &error);
    if (!snapshot) return process_error("workspace_snapshot_failed", error);
    sandbox::SandboxSpec spec;
    spec.metadata.identity.tenant_id = "local";
    spec.metadata.identity.task_id = "typed-process-tool";
    spec.metadata.identity.run_id = "toolbus";
    spec.provider = "bubblewrap";
    spec.workspace_base_digest = snapshot->digest;
    spec.command = command;
    spec.writable_mounts = {cfg.root.string() + ":/workspace"};
    spec.cpu_millis = static_cast<std::uint64_t>(args.value("cpu_millis", 30000));
    spec.memory_bytes = static_cast<std::uint64_t>(args.value("memory_bytes", 512U * 1024U * 1024U));
    spec.wall_time_ms = static_cast<std::uint64_t>(args.value("timeout_ms", 30000));
    spec.policy_revision = "typed-process-tools-v1";
    spec.memory_view_digest = "sha256:none";
    auto handle = provider->create(spec, &error);
    if (!handle) return process_error("sandbox_create_failed", error);
    auto result = provider->exec(*handle, &error);
    provider->destroy(*handle, nullptr);
    if (!result) return process_error("sandbox_exec_failed", error);
    return json{{"exit_code",result->exit_code},{"stdout",result->stdout_text},
                {"stderr",result->stderr_text},{"timed_out",result->timed_out},
                {"manifest",sandbox::encode(result->manifest)}};
}
std::vector<std::string> string_args(const json& args) {
    std::vector<std::string> out;
    if (args.contains("args")) for (const auto& item : args.at("args")) out.push_back(item.get<std::string>());
    return out;
}
std::optional<std::vector<sandbox::CredentialLease>> credentials(const json& j, std::string* error) {
    if(!j.contains("credential_ref")) return std::vector<sandbox::CredentialLease>{};
    std::shared_ptr<sandbox::CredentialBroker> broker;
    { std::lock_guard lock(credential_mutex); broker=credential_broker; }
    if(!broker) { if(error)*error="credential broker unavailable"; return std::nullopt; }
    return broker->resolve({j.at("credential_ref").get<std::string>()},error);
}
json typed_http(const json& j, bool download, const FsSandboxConfig& cfg,
                std::function<bool()> cancelled = {}) {
    WebHttpRequest request; request.url=j.at("url").get<std::string>(); request.method=j.value("method","GET");
    if(download && request.method!="GET" && request.method!="HEAD") return process_error("method_disallowed","Wget supports GET or HEAD");
    if(j.contains("headers")) for(auto it=j.at("headers").begin();it!=j.at("headers").end();++it) {
        if(web_http_sensitive_header(it.key())) return process_error("sensitive_header_forbidden",it.key());
        if(it.key().find('\r')!=std::string::npos || it.key().find('\n')!=std::string::npos ||
           !it.value().is_string() || it.value().get_ref<const std::string&>().find_first_of("\r\n")!=std::string::npos)
            return process_error("invalid_header",it.key());
        request.headers[it.key()]=it.value().get<std::string>();
    }
    if(j.contains("body_json")) { request.body=j.at("body_json").dump(); request.content_type="application/json"; }
    else request.body=j.value("body_text","");
    request.content_type=j.value("content_type",request.content_type);
    request.follow_redirects=j.value("follow_redirects",true);
    request.cancellation_requested=std::move(cancelled);
    if(j.contains("idempotency_key")) {
        const auto key=j.at("idempotency_key").get<std::string>();
        if(key.empty() || key.find_first_of("\r\n")!=std::string::npos)
            return process_error("invalid_idempotency_key","idempotency key is empty or unsafe");
        request.headers["Idempotency-Key"]=key;
    }
    std::string error; auto leases=credentials(j,&error);
    if(!leases) return process_error("credential_unavailable",error);
    if(!leases->empty()) {
        const auto auth=j.value("auth_type","bearer");
        if(auth=="bearer") request.headers["Authorization"]="Bearer "+leases->front().value;
        else if(auth=="cookie") request.headers["Cookie"]=leases->front().value;
        else if(auth=="basic") request.headers["Authorization"]="Basic "+base64(leases->front().value);
        else if(auth=="api_key_header") {
            const auto header=j.value("api_key_header","X-API-Key");
            if(web_http_sensitive_header(header) || header.find_first_of("\r\n")!=std::string::npos)
                return process_error("invalid_api_key_header",header);
            request.headers[header]=leases->front().value;
        }
        else return process_error("auth_type_unsupported",auth);
    }
    auto config=load_web_http_config_from_env();
    if(j.contains("timeout_ms")) config.timeout_ms=j.at("timeout_ms").get<int>();
    if(j.contains("max_bytes")) config.max_body_bytes=j.at("max_bytes").get<std::size_t>();
    fs::path partial;
    std::size_t partial_size=0;
    if(download && j.contains("output_path") && j.value("resume",false)) {
        json pe; auto target=fs_resolve_under_root(j.at("output_path").get<std::string>(),cfg.root,pe);
        if(!target)return pe;
        partial=target->string()+".part"; std::error_code x;
        if(fs::is_regular_file(partial,x)) { partial_size=fs::file_size(partial,x); request.headers["Range"]="bytes="+std::to_string(partial_size)+"-";
            std::ifstream etag(partial.string()+".etag");std::string value;std::getline(etag,value);if(!value.empty())request.headers["If-Range"]=value; }
    }
    fs::path transfer;
    std::ofstream transfer_stream;
    if(download && j.contains("output_path")) {
        json pe;auto target=fs_resolve_under_root(j.at("output_path").get<std::string>(),cfg.root,pe);
        if(!target)return pe;
        transfer=target->string()+".transfer";
        transfer_stream.open(transfer,std::ios::binary|std::ios::trunc);
        if(!transfer_stream)return process_error("open_failed","cannot open streaming transfer file");
        request.response_sink=[&](std::string_view chunk){
            transfer_stream.write(chunk.data(),static_cast<std::streamsize>(chunk.size()));
            return transfer_stream.good();
        };
    }
    const bool inherently_idempotent=request.method=="GET"||request.method=="HEAD"||request.method=="PUT"||
        request.method=="DELETE"||request.method=="OPTIONS";
    if(j.value("retry",0)>0 && !inherently_idempotent && !j.contains("idempotency_key"))
        return process_error("idempotency_key_required","retrying this method requires idempotency_key");
    const int max_attempts=std::max(1,std::min(j.value("retry",0)+1,6)); WebHttpResult result;
    int attempts=0;
    for(;attempts<max_attempts;++attempts) {
        if(attempts>0 && transfer_stream.is_open()) { transfer_stream.close();transfer_stream.open(transfer,std::ios::binary|std::ios::trunc); }
        result=web_http_request(request,config);
        const bool retryable=result.error_code=="timeout" || result.status==408 || result.status==429 || result.status>=500;
        if(!retryable || attempts+1>=max_attempts)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(2000,100*(1<<attempts))));
    }
    if(transfer_stream.is_open())transfer_stream.close();
    json out{{"status",result.status},{"final_url",result.final_url},{"content_type",result.content_type},
             {"redirects",result.redirects},{"attempts",attempts+1},{"truncated",result.truncated}};
    if(!result.error_code.empty()) out["error"]={{"code",result.error_code},{"status",result.error_http_status}};
    if(j.contains("expected_status")) {
        bool matched=false;
        if(j.at("expected_status").is_number_integer()) matched=result.status==j.at("expected_status").get<int>();
        else if(j.at("expected_status").is_array()) for(const auto& status:j.at("expected_status")) matched=matched||result.status==status.get<int>();
        if(!matched) out["error"]={{"code","unexpected_status"},{"status",result.status}};
    }
    if(j.value("fail_on_http_error",false) && result.status>=400)
        out["error"]={{"code","http_error"},{"status",result.status}};
    if(j.value("output_mode",std::string("inline"))=="object_store") {
        std::shared_ptr<distributed::ObjectStore> store; std::string tenant;
        {std::lock_guard lock(object_store_mutex);store=network_object_store;tenant=network_object_tenant;}
        if(!store)return process_error("object_store_unavailable","network ObjectStore is not configured");
        std::string store_error;auto ref=store->put(tenant,result.body,result.content_type.empty()?"application/octet-stream":result.content_type,{},&store_error);
        if(!ref)return process_error("object_store_put_failed",store_error);
        out["object_ref"]={{"tenant_id",ref->tenant_id},{"digest",ref->digest},{"size",ref->size},{"media_type",ref->media_type}};
        return out;
    }
    if(!download) { out["body"]=result.body; return out; }
    if(!j.contains("output_path")) return process_error("output_path_required","Wget requires output_path");
    json path_error; auto path=fs_resolve_under_root(j.at("output_path").get<std::string>(),cfg.root,path_error);
    if(!path) return path_error;
    std::error_code ec; if(fs::exists(*path,ec) && !j.value("overwrite",false)) return process_error("output_exists","overwrite=true required");
    if(result.truncated || !result.error_code.empty()) { if(result.error_code!="cancelled"){std::error_code x;fs::remove(transfer,x);} return out; }
    const auto tmp=path->string()+".part";
    const bool append=partial_size>0 && result.status==206;
    { std::ifstream source(transfer,std::ios::binary);std::ofstream stream(tmp,std::ios::binary|(append?std::ios::app:std::ios::trunc));
      stream<<source.rdbuf();if(!source||!stream){fs::remove(transfer,ec);return process_error("write_failed","streamed download write failed");} }
    fs::remove(transfer,ec);
    if(const auto etag=result.headers.find("ETag");etag!=result.headers.end()) {
        std::ofstream sidecar(tmp+".etag",std::ios::trunc); sidecar<<etag->second;
    }
    std::ifstream complete(tmp,std::ios::binary); const std::string bytes((std::istreambuf_iterator<char>(complete)),{});
    if(j.contains("expected_size") && bytes.size()!=j.at("expected_size").get<std::size_t>()) return process_error("size_mismatch","download size mismatch");
    if(j.contains("expected_digest")) { const auto expected=j.at("expected_digest").get<std::string>(); const auto colon=expected.find(':');
        auto actual=digest_bytes(bytes,colon==std::string::npos?"sha256":expected.substr(0,colon));
        if(!actual || *actual!=expected)return process_error("checksum_mismatch","download checksum mismatch"); }
    fs::rename(tmp,*path,ec); if(ec){fs::remove(tmp,ec);return process_error("rename_failed",ec.message());}
    fs::remove(tmp+".etag",ec); out["written"]=true; out["resumed"]=append; out["path"]=j.at("output_path"); out["bytes"]=bytes.size();
    out["digest"]=digest_bytes(bytes,"sha256").value_or(""); return out;
}
void register_fixed(ToolBus& bus, const FsSandboxConfig& cfg, std::string name,
                    std::string executable) {
    ToolMeta meta;
    meta.name = name;
    meta.description = "Run " + name + " with an argument array in a deny-network Bubblewrap workspace sandbox.";
    meta.schema = json::parse(R"({"type":"object","properties":{"args":{"type":"array","items":{"type":"string"}},"timeout_ms":{"type":"integer","minimum":1},"cpu_millis":{"type":"integer","minimum":1},"memory_bytes":{"type":"integer","minimum":1}},"required":["args"]})");
    meta.side_effect = ToolSideEffect::Write;
    meta.permission_targets.push_back({ToolMeta::PermissionTargetKind::FilesystemWrite,{},cfg.root.string(),{}});
    bus.register_local_tool(name, [cfg, executable=std::move(executable)](const json& j) {
        auto command = std::vector<std::string>{executable};
        auto tail = string_args(j); command.insert(command.end(),tail.begin(),tail.end());
        return run_sandboxed(cfg,command,j);
    }, meta);
}
}

void register_builtin_process_tools_if_configured(ToolBus& bus) {
    if (bus.get_tool_info("Bash")) return;
    auto cfg = load_fs_sandbox_config_from_env();
    if (!cfg) return;
    register_fixed(bus,*cfg,"Python","/usr/bin/python3");
    register_fixed(bus,*cfg,"CMake","/usr/bin/cmake");
    register_fixed(bus,*cfg,"Make","/usr/bin/make");
    ToolMeta bash;
    bash.name="Bash"; bash.description="Run a command in a deny-network Bubblewrap workspace sandbox.";
    bash.schema=json::parse(R"({"type":"object","properties":{"command":{"type":"string"},"timeout_ms":{"type":"integer","minimum":1}},"required":["command"]})");
    bash.side_effect=ToolSideEffect::Write;
    bash.permission_targets.push_back({ToolMeta::PermissionTargetKind::FilesystemWrite,{},cfg->root.string(),{}});
    bus.register_local_tool("Bash",[cfg=*cfg](const json& j){
        return run_sandboxed(cfg,{"/bin/bash","-lc","cd /workspace && "+j.at("command").get<std::string>()},j);
    },bash);
    auto network = [](std::string name, bool download) {
        ToolMeta meta; meta.name=name;
        meta.description=download?"Download one URL through the policy-enforcing WebFetch transport.":"Perform a typed HTTP GET through the policy-enforcing WebFetch transport.";
        meta.schema=json::parse(R"({"type":"object","properties":{"url":{"type":"string"},"method":{"type":"string","enum":["GET","HEAD","POST","PUT","PATCH","DELETE","OPTIONS"]},"headers":{"type":"object","additionalProperties":true},"body_text":{"type":"string"},"body_json":{},"content_type":{"type":"string"},"credential_ref":{"type":"string"},"auth_type":{"type":"string","enum":["bearer","cookie","basic","api_key_header"]},"api_key_header":{"type":"string"},"idempotency_key":{"type":"string"},"follow_redirects":{"type":"boolean"},"timeout_ms":{"type":"integer","minimum":1},"max_bytes":{"type":"integer","minimum":1},"retry":{"type":"integer","minimum":0,"maximum":5},"expected_status":{},"fail_on_http_error":{"type":"boolean"},"output_mode":{"type":"string","enum":["inline","object_store"]},"output_path":{"type":"string"},"overwrite":{"type":"boolean"},"resume":{"type":"boolean"},"expected_size":{"type":"integer","minimum":0},"expected_digest":{"type":"string"}},"required":["url"]})");
        meta.side_effect=ToolSideEffect::ReadOnly;
        meta.permission_targets.push_back({ToolMeta::PermissionTargetKind::Network,"url",{}, {}});
        return meta;
    };
    bus.register_local_tool("Curl",[cfg=*cfg](const json& j){return typed_http(j,false,cfg);},network("Curl",false));
    auto wget_meta=network("Wget",true); wget_meta.side_effect=ToolSideEffect::Write;
    wget_meta.permission_targets.push_back({ToolMeta::PermissionTargetKind::FilesystemWrite,"output_path",cfg->root.string(),{}});
    bus.register_cancellable_local_tool("Wget",[cfg=*cfg](const json& j,const ToolCallControl& control){
        return typed_http(j,true,cfg,[control]{return control.should_stop();});},wget_meta);
    bus.register_tool_alias("bash","Bash"); bus.register_tool_alias("curl","Curl");
    bus.register_tool_alias("wget","Wget"); bus.register_tool_alias("python3","Python");
    bus.register_tool_alias("cmake","CMake"); bus.register_tool_alias("make","Make");
}

void configure_network_tool_credentials(std::shared_ptr<sandbox::CredentialBroker> broker) {
    std::lock_guard lock(credential_mutex); credential_broker=std::move(broker);
}
void configure_network_tool_object_store(std::shared_ptr<distributed::ObjectStore> store,
                                         std::string tenant_id) {
    if(tenant_id.empty())throw std::invalid_argument("network ObjectStore tenant is required");
    std::lock_guard lock(object_store_mutex);network_object_store=std::move(store);network_object_tenant=std::move(tenant_id);
}
}
