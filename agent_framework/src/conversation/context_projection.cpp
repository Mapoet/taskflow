#include "agent/conversation/context_projection.hpp"
#include "agent/contracts/contract.hpp"
#include <set>
namespace agent_framework::conversation {
std::optional<ContextProjectionManifest> ContextProjector::build(ContextProjectionManifest v,std::string*e){
 if(v.identity.tenant_id.empty()||v.identity.conversation_id.empty()||v.turn_id.empty()||v.revision==0||v.profile_revision_digest.empty()||v.prompt_revision_digest.empty()){if(e)*e="context_projection_identity_required";return std::nullopt;}
 std::set<std::string> kinds;for(const auto&s:v.segments){if(s.kind.empty()||s.reference.empty()||s.digest.empty()||s.authority.empty()){if(e)*e="context_segment_binding_required";return std::nullopt;}if(!kinds.insert(s.kind).second){if(e)*e="duplicate_context_segment_kind";return std::nullopt;}}
 v.digest=contracts::canonical_digest(encode(v)).value_or("");if(v.digest.empty()){if(e)*e="context_projection_digest_failed";return std::nullopt;}return v;
}
bool ContextProjector::validate_boundary(const ContextProjectionManifest&before,const CompactBoundaryRecord&b,std::string*e){
 if(b.identity.tenant_id!=before.identity.tenant_id||b.identity.conversation_id!=before.identity.conversation_id||b.turn_id!=before.turn_id||b.revision==0||b.summary_digest.empty()){if(e)*e="compact_boundary_binding_mismatch";return false;}
 if(b.post_tokens>b.pre_tokens){if(e)*e="compact_boundary_token_growth";return false;}
 for(const auto&s:before.segments)if(s.mandatory&&(s.kind=="contract"||s.kind=="policy"||s.kind=="citation")&&!s.truncation_reason.empty()){if(e)*e="mandatory_context_segment_truncated";return false;}
 return true;
}}
