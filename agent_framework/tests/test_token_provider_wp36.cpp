#include <agent/agent_client/token_provider.hpp>
#include <cassert>
#include <iostream>
using namespace agent_framework;
int main(){auto s=std::make_shared<MemoryCredentialStore>();int calls=0;s->save({"old",std::string("refresh"),std::chrono::system_clock::now()-std::chrono::seconds(1)});RefreshingTokenProvider p(s,[&](const OAuthToken&){++calls;return OAuthToken{"new",std::string("refresh"),std::chrono::system_clock::now()+std::chrono::hours(1)};});assert(p.access_token()=="new"&&calls==1);assert(p.access_token()=="new"&&calls==1);std::cout<<"test_token_provider_wp36: ok\n";}
