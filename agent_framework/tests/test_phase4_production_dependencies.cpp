#include <cassert>

#include "agent/harness/production_dependencies.hpp"

namespace {
class Sandbox final : public agent_framework::sandbox::SandboxProvider {
public:
 std::string id()const override{return "test";}std::string version()const override{return "v1";}
 bool available(std::string*)const override{return available_;}
 std::optional<agent_framework::sandbox::SandboxHandle> create(const agent_framework::sandbox::SandboxSpec&,std::string*)override{return {};}
 std::optional<agent_framework::sandbox::ExecResult> exec(const agent_framework::sandbox::SandboxHandle&,std::string*)override{return {};}
 bool destroy(const agent_framework::sandbox::SandboxHandle&,std::string*)override{return true;}
 bool available_{true};
};}
int main(){using namespace agent_framework::harness;ProductionCompositionDependencies d;auto empty=validate_production_dependencies(d);assert(!empty.ready&&empty.issues.size()>=20);Sandbox sandbox;d.sandbox_provider=&sandbox;d.identity_verifier_manifest_digest="sha256:id";d.policy_manifest_digest="sha256:policy";d.configuration_revision="r1";auto partial=validate_production_dependencies(d);assert(!partial.ready);sandbox.available_=false;auto unavailable=validate_production_dependencies(d);bool found=false;for(const auto&i:unavailable.issues)if(i.code=="sandbox_provider_unavailable")found=true;assert(found);}
