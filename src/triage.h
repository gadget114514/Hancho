#pragma once

#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace pc {

struct TriageInputs {
    nlohmann::json jobs;
    nlohmann::json btFailures;
    nlohmann::json btStates;
    nlohmann::json config;
    std::string now;
};

nlohmann::json BuildTriageCandidates(const TriageInputs& in);
nlohmann::json BuildProcessTriageCandidates(const nlohmann::json& processes);
std::string TriageCardIdFromKey(const std::string& key);
std::string TriageTimeoutPattern(const nlohmann::json& job);
std::vector<std::string> MergeTriageCards(nlohmann::json& store,
                                          const nlohmann::json& fresh,
                                          const std::string& now);
void ApplyTriageKindSettings(nlohmann::json& store, nlohmann::json& fresh,
                             const nlohmann::json& config, const std::string& now);
bool TriageNeedsLlm(const nlohmann::json& card,
                    const std::string& mode,
                    const std::string& llmMode);
std::string TriageAutoChoice(const nlohmann::json& store,
                             const nlohmann::json& card,
                             const std::string& mode,
                             std::string& outBy,
                             std::string& outWhyNot);
std::string BuildTriageAdvicePacket(const nlohmann::json& card,
                                    const std::string& language);
const char* TriageAdviceOutputSchema();
bool ParseTriageAdvice(const std::string& resultJson,
                       const nlohmann::json& card,
                       nlohmann::json& outAdvice,
                       std::string& err);

// Instructions for a non-interactive supervisor worker that handles the given pending cards and
// records each decision with `<cli> triage decide`. cli is the full hancho-cli invocation including
// --data-dir, so the worker talks to the same core.
// Follows a handled card to the end of its lifecycle, including the branches it leads to.
// jobs is the full taskqueue list ("jobs" array), btStates maps BT id -> tree state.
// Returns {"state": "rerunning" | "succeeded" | "failed_again" | "cancelled" | "not_rerun" | "gone",
//          "detail", "nextKey" (failed again: the key of the card that follows),
//          "branch": {"total", "succeeded", "running", "failed", "blocked", "cancelled",
//                     "jobs": [{"id","state","key"}], "done"},   (jobs that depend on this one)
//          "bt": {"id", "state"},                              (the tree the job or node belongs to)
//          "complete": true when the job and every branch reached a final state}
// or null when there is nothing to follow (still pending and untouched, ignored, no target).
nlohmann::json TriageOutcome(const nlohmann::json& card, const nlohmann::json& jobs,
                             const nlohmann::json& btStates);

// interactive = true builds the prompt a person pastes into a conversation (the Claude desktop app):
// no result file, no "exit when done", and it may ask the person before acting.
std::string BuildTriageHandoffInstructions(const nlohmann::json& cards,
                                           const std::string& cli,
                                           const std::string& language,
                                           bool interactive = false);

} // namespace pc
