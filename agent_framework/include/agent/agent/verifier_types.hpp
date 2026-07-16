/**
 * @file verifier_types.hpp
 * @brief WP2.8 Verifier JSON v1 types, parsing, and gate logic (phase-2-wp8.md)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_VERIFIER_TYPES_H__
#define __AGENT_VERIFIER_TYPES_H__

#include <agent/core/types.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent_framework {

/** @brief Single issue from Verifier LLM output (wp8 §4.2). */
struct VerifierIssue {
    std::string code;
    std::string severity;  // info | warn | block
    std::string detail;
};

/** @brief Normalized Verifier LLM result after parse + §4 defaults. */
struct VerifierResult {
    bool ok = false;
    std::vector<VerifierIssue> issues;
    /** One of: pass | retry_main | pass_through | abort */
    std::string suggested_action;
};

/**
 * @brief Parse Verifier model raw response (UTF-8 JSON body, optional markdown fence).
 * @param raw Model output text
 * @param verifier_retry_count Current AgentThreadState::verifier_retry_count for §4 default
 * @param max_retries AGENT_VERIFIER_MAX_RETRIES semantics: extra MAIN attempts allowed
 * On failure: returns §8 synthetic (pass_through + verifier_parse_error).
 */
VerifierResult parse_verifier_response(std::string_view raw,
                                      int verifier_retry_count,
                                      int max_retries);

/** @brief §6.3 timeout synthetic result (pass_through, no retry_main). */
VerifierResult make_verifier_timeout_result(std::string_view detail);

enum class VerifierGateKind {
    PublishPass,         // ok to publish; verifier_ok true
    PublishPassThrough,  // publish draft; verifier_ok false
    RetryMain,           // inject FIX hint and rerun MAIN
    Abort                // verifier_abort; CLI exit 4 / A2A FAILED
};

/** @brief Outcome of §5.1 gate in deterministic order. */
struct VerifierGateOutcome {
    VerifierGateKind kind = VerifierGateKind::PublishPassThrough;
    bool verifier_ok = false;
    /** Effective action for logging: pass | retry_main | pass_through | abort */
    std::string effective_action;
    /** True when suggested_action was not in §4.3 and was normalized (wp8 §5.1). */
    bool action_unknown_normalized = false;
};

/**
 * @brief Apply wp8 §5.1 gate to an already-normalized VerifierResult.
 * @param r Parsed/normalized result
 * @param verifier_retry_count BEFORE increment for this retry decision (wp8 §5)
 * @param max_retries AGENT_VERIFIER_MAX_RETRIES (default 1 = one extra MAIN)
 */
VerifierGateOutcome apply_verifier_gate(const VerifierResult& r,
                                       int verifier_retry_count,
                                       int max_retries);

/**
 * @brief Build §5.3 system Message content (English, byte caps). Caller sets role=system.
 * @param issues From VerifierResult (issues array compact JSON <=1536 B inside template)
 */
std::string build_verifier_fix_system_message(const std::vector<VerifierIssue>& issues);

} // namespace agent_framework

#endif // __AGENT_VERIFIER_TYPES_H__
