#include "chain_guard.h"

#ifndef _WIN32

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

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

std::string BuildHint(const std::vector<std::string>& parts) {
    std::ostringstream oss;
    oss << "Register one job per command with taskqueue_add (timeouts are per job):\n";
    oss << "  {\"taskqueue\":\"main\",\"jobs\":[\n";
    for (size_t i = 0; i < parts.size(); ++i) {
        oss << "    {\"id\":\"step" << (i + 1) << "\",\"command\":\""
            << (parts[i].empty() ? std::string("<command>") : parts[i])
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
    std::string inner = Trim(command);
    if (inner.empty()) return true;

    // POSIX wait loops. `sleep` alone is not a loop, but a while/until body
    // containing sleep or a test is polling and should be a dependency.
    std::string lower = Lower(inner);
    if ((lower.find("while ") != std::string::npos || lower.rfind("while", 0) == 0 ||
         lower.find("until ") != std::string::npos || lower.rfind("until", 0) == 0) &&
        (lower.find("sleep") != std::string::npos || lower.find("test ") != std::string::npos ||
         lower.find("[ ") != std::string::npos)) {
        outError = "Waiting loop detected. Do not poll for another process; declare a dependency instead.\n"
                   "Use dependsOn in taskqueue_add.\n" + BuildHint({inner});
        return false;
    }
    if (lower.find(" in $(seq ") != std::string::npos ||
        lower.find(" in `seq ") != std::string::npos) {
        outError = "Waiting loop detected (seq loop). Use dependsOn in taskqueue_add.\n" +
                   BuildHint({inner});
        return false;
    }

    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (inSingle) {
            if (c == '\'') inSingle = false;
            continue;
        }
        if (inDouble) {
            if (c == '\\' && i + 1 < inner.size()) { ++i; continue; }
            if (c == '"') inDouble = false;
            continue;
        }
        if (c == '\\' && i + 1 < inner.size()) { ++i; continue; }
        if (c == '\'') { inSingle = true; continue; }
        if (c == '"') { inDouble = true; continue; }

        std::string op;
        if (c == '&') {
            if (i + 1 < inner.size() && inner[i + 1] == '&') { op = "&&"; ++i; }
            else op = "&";
        } else if (c == '|') {
            if (i + 1 < inner.size() && inner[i + 1] == '|') { op = "||"; ++i; }
            else continue; // a single pipe is allowed
        } else if (c == '\n' || c == '\r') {
            op = "newline";
        } else if (c == ';') {
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
            << "A job must be exactly one command. " << BuildHint(parts);
        outError = oss.str();
        return false;
    }

    return true;
}

} // namespace pc

#endif // !_WIN32
