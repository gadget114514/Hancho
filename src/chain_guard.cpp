#include "chain_guard.h"
#include <string>
#include <vector>
#include <sstream>
#include <cctype>

namespace pc {

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool StartsWithCI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

std::string StripSurroundingQuotes(const std::string& s) {
    std::string t = Trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

// Remove an outer `cmd /c "..."` or `powershell -Command "..."` wrapper.
// Sets isPowerShell when the wrapper was a PowerShell one.
std::string StripOuterWrapper(const std::string& command, bool& isPowerShell) {
    isPowerShell = false;
    std::string s = Trim(command);
    if (s.empty()) return s;

    if (StartsWithCI(s, "cmd")) {
        size_t i = 3;
        if (i + 4 <= s.size() && Lower(s.substr(i, 4)) == ".exe") i += 4;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i + 2 <= s.size() && (s[i] == '/' || s[i] == '-') &&
            (std::tolower(static_cast<unsigned char>(s[i + 1])) == 'c')) {
            i += 2;
            std::string rest = Trim(s.substr(i));
            return StripSurroundingQuotes(rest);
        }
        return s;
    }

    if (StartsWithCI(s, "powershell") || StartsWithCI(s, "pwsh")) {
        std::string lower = Lower(s);
        size_t cmdPos = lower.find("-command");
        if (cmdPos == std::string::npos) cmdPos = lower.find("-c ");
        if (cmdPos != std::string::npos) {
            size_t argStart = cmdPos;
            while (argStart < s.size() && !std::isspace(static_cast<unsigned char>(s[argStart]))) argStart++;
            isPowerShell = true;
            return StripSurroundingQuotes(s.substr(argStart));
        }
        isPowerShell = true;
        return s;
    }
    return s;
}

std::vector<std::string> SplitOnOperator(const std::string& inner, const std::string& op) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t found = inner.find(op, pos);
        if (found == std::string::npos) {
            parts.push_back(Trim(inner.substr(pos)));
            break;
        }
        parts.push_back(Trim(inner.substr(pos, found - pos)));
        pos = found + op.size();
    }
    return parts;
}

std::string BuildTaskqueueHint(const std::vector<std::string>& parts) {
    std::ostringstream oss;
    oss << "Register one job per command with taskqueue_add (timeouts are per job and are never summed):\n";
    oss << "  {\"taskqueue\":\"main\",\"jobs\":[\n";
    for (size_t i = 0; i < parts.size(); ++i) {
        oss << "    {\"id\":\"step" << (i + 1) << "\",\"command\":\"" << (parts[i].empty() ? std::string("<command>") : parts[i])
            << "\",\"summary\":\"<one line summary>\",\"timeoutSec\":1800";
        if (i > 0) oss << ",\"dependsOn\":[\"step" << i << "\"]";
        oss << "}" << (i + 1 < parts.size() ? "," : "") << "\n";
    }
    oss << "  ]}\n";
    return oss.str();
}

} // namespace

bool CheckChainGuard(const std::string& command, std::string& outError) {
    outError.clear();
    std::string trimmed = Trim(command);
    if (trimmed.empty()) return true;

    bool isPowerShell = false;
    std::string inner = StripOuterWrapper(trimmed, isPowerShell);

    // Wait loop detection (simple pattern match as allowed by the design).
    std::string lower = Lower(inner);
    if ((lower.find("while") != std::string::npos || lower.find("until") != std::string::npos) &&
        (lower.find("test-path") != std::string::npos || lower.find("start-sleep") != std::string::npos)) {
        outError = "Waiting loop detected. Do not poll for another process; declare a dependency instead.\n"
                   "Use dependsOn in taskqueue_add (e.g. \"dependsOn\":[\"otherJobId\"]).\n" +
                   BuildTaskqueueHint({inner});
        return false;
    }
    if (lower.find(":loop") != std::string::npos && lower.find("goto") != std::string::npos) {
        outError = "Waiting loop detected (goto loop). Do not poll for another process; declare a dependency instead.\n"
                   "Use dependsOn in taskqueue_add (e.g. \"dependsOn\":[\"otherJobId\"]).\n" +
                   BuildTaskqueueHint({inner});
        return false;
    }

    bool inStr = false;
    bool escaped = false;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '^') { ++i; continue; }

        std::string op;
        if (c == '&') {
            if (i + 1 < inner.size() && inner[i + 1] == '&') { op = "&&"; i++; }
            else op = "&";
        } else if (c == '|') {
            if (i + 1 < inner.size() && inner[i + 1] == '|') { op = "||"; i++; }
            else continue; // single pipe is allowed
        } else if (c == '\n' || c == '\r') {
            op = "newline";
        } else if (c == ';' && isPowerShell) {
            op = ";";
        }
        if (op.empty()) continue;

        std::vector<std::string> parts;
        if (op == "&&" || op == "||") parts = SplitOnOperator(inner, op);
        else if (op == "&") parts = SplitOnOperator(inner, "&");
        else if (op == ";") parts = SplitOnOperator(inner, ";");
        else parts = {Trim(inner.substr(0, i)), Trim(inner.substr(i + 1))};

        std::ostringstream oss;
        oss << "Chained command detected: operator '" << op << "' at position " << i << ".\n"
            << "A job must be exactly one command. " << BuildTaskqueueHint(parts);
        outError = oss.str();
        return false;
    }

    return true;
}

} // namespace pc
