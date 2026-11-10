#pragma once

// Remote host registry (doc/design-v3.md 2.5, step C).
//
// Hosts live in <data-dir>/hosts.json:
//
//   { "schema": "hancho.hosts/1",
//     "hosts": [ { "name": "buildbox", "address": "127.0.0.1:9137",
//                  "token_env": "HANCHO_TOKEN_BUILDBOX",
//                  "token_credential": "Hancho/host/buildbox" } ] }
//
// The token itself is never written to hosts.json. ResolveHostToken() reads it
// from the environment first and then from the Windows Credential Manager.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pc {

struct RemoteHost {
    std::string name;
    std::string address;        // host:port, [ipv6]:port, or ws://host:port
    std::string tokenEnv;       // environment variable holding the token
    std::string tokenCredential;// Windows Credential Manager target
    bool enabled = true;
};

// Deterministic default environment variable for a host name.
// "buildbox" -> "HANCHO_TOKEN_BUILDBOX".
std::string DefaultHostTokenEnv(const std::string& hostName);

// Default Windows Credential Manager target: "Hancho/host/<name>".
std::string DefaultHostTokenCredential(const std::string& hostName);

// Accepts "host:port", "[::1]:9137" and "ws://host:port". "wss://" is rejected
// because TLS is left to the SSH tunnel. A missing port becomes 9137.
bool ParseRemoteAddress(const std::string& address, std::string& outHost,
                        uint16_t& outPort, std::string& outError);

// Environment lookup with an injectable override, used by tests.
using EnvLookup = std::function<std::string(const std::string&)>;

// Reads the token for a host: token_env (or the default name), then the
// platform credential store. Returns an empty string when it is not available.
std::string ResolveHostToken(const RemoteHost& host);
std::string ResolveHostToken(const RemoteHost& host, const EnvLookup& env);

// Token for the local remote listener. HANCHO_REMOTE_TOKEN, then the Windows
// Credential Manager target "Hancho/server".
std::string ResolveServerToken();

// True when address is a loopback literal (127.0.0.0/8, ::1, localhost).
bool IsLoopbackHost(const std::string& host);

// A host entry is valid when the name is non-empty and contains only
// [A-Za-z0-9._-], and the address parses.
bool ValidateRemoteHost(const RemoteHost& host, std::string& outError);

class HostRegistry {
public:
    // <data-dir>/hosts.json
    static std::wstring FilePathWide();

    // Missing file is not an error: the registry simply starts empty.
    bool Load(std::string& outError);
    bool Save(std::string& outError) const;

    bool Add(const RemoteHost& host, std::string& outError);
    bool Remove(const std::string& name, std::string& outError);
    bool Get(const std::string& name, RemoteHost& outHost) const;
    std::vector<RemoteHost> List() const;
    bool Empty() const { return hosts_.empty(); }

    // {"ok":true,"hosts":[...]} with token presence but never the token itself.
    std::string ToJson(bool includeResolvedTokenPresence) const;

private:
    std::vector<RemoteHost> hosts_;
};

} // namespace pc
