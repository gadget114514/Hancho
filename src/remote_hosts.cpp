#include "remote_hosts.h"

#include "common.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <wincred.h>
#endif

namespace pc {

namespace {

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string UpperAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string ReadCredentialTarget(const std::string& target) {
#ifdef _WIN32
    if (target.empty()) return "";
    std::wstring targetW = Utf8ToWide(target);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(targetW.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred) return "";
    std::string out;
    if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
        out.assign(reinterpret_cast<const char*>(cred->CredentialBlob),
                   static_cast<size_t>(cred->CredentialBlobSize));
    }
    CredFree(cred);
    return out;
#else
    (void)target;
    return "";
#endif
}

std::string HostTokenEnvName(const RemoteHost& host) {
    return host.tokenEnv.empty() ? DefaultHostTokenEnv(host.name) : host.tokenEnv;
}

} // namespace

std::string DefaultHostTokenEnv(const std::string& hostName) {
    std::string name = UpperAscii(hostName);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return "HANCHO_TOKEN_" + name;
}

std::string DefaultHostTokenCredential(const std::string& hostName) {
    return "Hancho/host/" + hostName;
}

bool ParseRemoteAddress(const std::string& address, std::string& outHost,
                        uint16_t& outPort, std::string& outError) {
    outHost.clear();
    outPort = 9137;
    outError.clear();

    std::string s = Trim(address);
    if (s.empty()) {
        outError = "address is empty";
        return false;
    }
    std::string lower = LowerAscii(s);
    if (lower.rfind("wss://", 0) == 0) {
        outError = "wss:// is not supported; TLS is left to the SSH tunnel (use ws:// or host:port)";
        return false;
    }
    if (lower.rfind("ws://", 0) == 0) s = s.substr(5);
    else if (lower.rfind("http://", 0) == 0) s = s.substr(7);

    std::string portText;
    if (!s.empty() && s.front() == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos) {
            outError = "unterminated IPv6 address: " + address;
            return false;
        }
        outHost = s.substr(1, close - 1);
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') {
                outError = "unexpected text after IPv6 address: " + address;
                return false;
            }
            portText = s.substr(close + 2);
        }
    } else {
        size_t colon = s.rfind(':');
        if (colon != std::string::npos && s.find(':') == colon) {
            outHost = s.substr(0, colon);
            portText = s.substr(colon + 1);
        } else {
            outHost = s;
        }
    }

    if (outHost.empty()) {
        outError = "host is empty in address: " + address;
        return false;
    }
    if (!portText.empty()) {
        int port = 0;
        try {
            size_t used = 0;
            port = std::stoi(portText, &used);
            if (used != portText.size()) throw std::invalid_argument("port");
        } catch (...) {
            outError = "invalid port in address: " + address;
            return false;
        }
        if (port <= 0 || port > 65535) {
            outError = "port out of range in address: " + address;
            return false;
        }
        outPort = static_cast<uint16_t>(port);
    }
    return true;
}

std::string ResolveHostToken(const RemoteHost& host) {
    return ResolveHostToken(host, [](const std::string& name) -> std::string {
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : std::string();
    });
}

std::string ResolveHostToken(const RemoteHost& host, const EnvLookup& env) {
    std::string token = env ? env(HostTokenEnvName(host)) : std::string();
    if (!token.empty()) return token;

    std::string target = host.tokenCredential.empty()
                             ? DefaultHostTokenCredential(host.name)
                             : host.tokenCredential;
    token = ReadCredentialTarget(target);
    if (!token.empty()) return token;

    // Last-resort shared token, useful for an SSH tunnel to a single host.
    if (env) token = env("HANCHO_TOKEN");
    return token;
}

std::string ResolveServerToken() {
    const char* value = std::getenv("HANCHO_REMOTE_TOKEN");
    if (value && *value) return std::string(value);
    return ReadCredentialTarget("Hancho/server");
}

bool IsLoopbackHost(const std::string& host) {
    std::string lower = LowerAscii(Trim(host));
    if (lower == "localhost" || lower == "::1") return true;
    if (lower.rfind("127.", 0) == 0) return true;
    return false;
}

bool ValidateRemoteHost(const RemoteHost& host, std::string& outError) {
    outError.clear();
    if (host.name.empty()) {
        outError = "host name is required";
        return false;
    }
    for (char c : host.name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') {
            outError = "host name may only contain letters, digits, '.', '_' and '-'";
            return false;
        }
    }
    std::string parsedHost;
    uint16_t port = 0;
    return ParseRemoteAddress(host.address, parsedHost, port, outError);
}

std::wstring HostRegistry::FilePathWide() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"hosts.json");
}

bool HostRegistry::Load(std::string& outError) {
    outError.clear();
    hosts_.clear();
    const std::string text = ReadFileUtf8Wide(FilePathWide());
    if (Trim(text).empty()) return true;

    nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        outError = "hosts.json is not valid JSON";
        return false;
    }

    const nlohmann::json* list = nullptr;
    if (doc.is_array()) {
        list = &doc;
    } else if (doc.is_object()) {
        auto it = doc.find("hosts");
        if (it != doc.end()) {
            if (!it->is_array()) {
                outError = "hosts.json: \"hosts\" must be an array";
                return false;
            }
            list = &*it;
        }
    }
    if (!list) return true;

    for (const auto& item : *list) {
        if (!item.is_object()) {
            outError = "hosts.json: every host must be an object";
            return false;
        }
        RemoteHost host;
        host.name = item.value("name", std::string());
        host.address = item.value("address", std::string());
        host.tokenEnv = item.value("token_env", std::string());
        host.tokenCredential = item.value("token_credential", std::string());
        host.enabled = item.value("enabled", true);

        std::string err;
        if (!ValidateRemoteHost(host, err)) {
            outError = "hosts.json: host '" + host.name + "': " + err;
            return false;
        }
        bool duplicate = false;
        for (const auto& existing : hosts_) {
            if (existing.name == host.name) { duplicate = true; break; }
        }
        if (duplicate) {
            outError = "hosts.json: duplicate host name '" + host.name + "'";
            return false;
        }
        hosts_.push_back(std::move(host));
    }
    return true;
}

bool HostRegistry::Save(std::string& outError) const {
    outError.clear();
    nlohmann::json doc;
    doc["schema"] = "hancho.hosts/1";
    doc["hosts"] = nlohmann::json::array();
    for (const auto& host : hosts_) {
        nlohmann::json item;
        item["name"] = host.name;
        item["address"] = host.address;
        if (!host.tokenEnv.empty()) item["token_env"] = host.tokenEnv;
        if (!host.tokenCredential.empty()) item["token_credential"] = host.tokenCredential;
        if (!host.enabled) item["enabled"] = false;
        doc["hosts"].push_back(std::move(item));
    }
    EnsureDirectoryWide(GetEffectiveDataDirectoryWide());
    WriteFileAtomicWide(FilePathWide(), doc.dump(2) + "\n");
    return true;
}

bool HostRegistry::Add(const RemoteHost& host, std::string& outError) {
    outError.clear();
    if (!ValidateRemoteHost(host, outError)) return false;
    for (auto& existing : hosts_) {
        if (existing.name == host.name) {
            existing = host;
            return Save(outError);
        }
    }
    hosts_.push_back(host);
    return Save(outError);
}

bool HostRegistry::Remove(const std::string& name, std::string& outError) {
    outError.clear();
    auto it = std::find_if(hosts_.begin(), hosts_.end(),
                           [&name](const RemoteHost& h) { return h.name == name; });
    if (it == hosts_.end()) {
        outError = "host not found: " + name;
        return false;
    }
    hosts_.erase(it);
    return Save(outError);
}

bool HostRegistry::Get(const std::string& name, RemoteHost& outHost) const {
    for (const auto& host : hosts_) {
        if (host.name == name) {
            outHost = host;
            return true;
        }
    }
    return false;
}

std::vector<RemoteHost> HostRegistry::List() const {
    return hosts_;
}

std::string HostRegistry::ToJson(bool includeResolvedTokenPresence) const {
    nlohmann::json out;
    out["ok"] = true;
    out["schema"] = "hancho.hosts/1";
    out["file"] = WideToUtf8(FilePathWide());
    out["hosts"] = nlohmann::json::array();
    for (const auto& host : hosts_) {
        nlohmann::json item;
        item["name"] = host.name;
        item["address"] = host.address;
        item["enabled"] = host.enabled;
        item["token_env"] = HostTokenEnvName(host);
        item["token_credential"] = host.tokenCredential.empty()
                                       ? DefaultHostTokenCredential(host.name)
                                       : host.tokenCredential;
        if (includeResolvedTokenPresence) {
            item["token_present"] = !ResolveHostToken(host).empty();
        }
        std::string parsedHost;
        uint16_t port = 0;
        std::string err;
        item["valid"] = ParseRemoteAddress(host.address, parsedHost, port, err);
        if (!item["valid"].get<bool>()) item["address_error"] = err;
        out["hosts"].push_back(std::move(item));
    }
    return out.dump();
}

} // namespace pc
