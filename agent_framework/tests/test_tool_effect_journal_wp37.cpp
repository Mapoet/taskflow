#include <agent/toolbus/tool_effect_journal.hpp>
#include <cassert>
#include <iostream>
using namespace agent_framework; int main(){ToolEffectJournal j;ToolEffectRecord r;r.idempotency_key="k";assert(j.start(r));assert(!j.start(r));assert(j.complete("k","digest"));assert(j.commit("k"));assert(j.recoverable().empty());std::cout<<"test_tool_effect_journal_wp37: ok\n";}
