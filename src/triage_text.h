#pragma once

#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace pc {

// Translate fixed triage vocabulary. Unknown values are returned unchanged.
std::string TriageLabel(const std::string& language, const std::string& type,
                        const std::string& value, const std::string& fallback = "");

// Format one structured info item. Evidence/output are split into rows so UIs can
// keep each log line independently readable. Legacy string items are unchanged.
std::vector<std::string> TriageFormatInfoRows(const std::string& language,
                                              const nlohmann::json& item);
std::string TriageFormatInfo(const std::string& language, const nlohmann::json& item);

std::string TriageFormatDuration(const std::string& language, long long seconds);
std::string TriageFormatRationale(const std::string& language, const std::string& key,
                                  const nlohmann::json& params,
                                  const std::string& fallback = "");

std::string TriageCandidateLabel(const std::string& language, const std::string& id,
                                 const std::string& fallback = "");
std::string TriageCandidateEffect(const std::string& language, const std::string& id,
                                  const std::string& fallback = "");

} // namespace pc
