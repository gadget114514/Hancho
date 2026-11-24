#include "direction_store.h"

#include "common.h"

#include <cctype>
#include <exception>
#include <filesystem>

namespace pc {

namespace {

std::wstring RecordPathWide(const std::wstring& dir, const std::string& id) {
    return PathJoinWide(dir, Utf8ToWide(id + ".json"));
}

bool ParseAutoNumber(const std::string& id, long long& outN) {
    const std::string prefix = DirectionStore::kIdPrefix;
    if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) return false;
    long long n = 0;
    for (size_t i = prefix.size(); i < id.size(); ++i) {
        const char c = id[i];
        if (c < '0' || c > '9') return false;
        if (n > 1000000000000LL) return false;
        n = n * 10 + (c - '0');
    }
    outN = n;
    return true;
}

} // namespace

void DirectionStore::Open(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    dir_ = dir;
    records_.clear();
    EnsureDirectoryWide(dir_);
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir_), ec);
    if (ec) return;
    for (const auto& entry : it) {
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) continue;
        const std::filesystem::path p = entry.path();
        if (p.extension() != ".json") continue;
        const std::string id = p.stem().string();
        nlohmann::json record = nlohmann::json::parse(ReadFileUtf8Wide(p.wstring()), nullptr, false);
        if (record.is_discarded() || !record.is_object()) continue;
        if (!record.contains("id") || !record["id"].is_string()) continue;
        records_[id] = std::move(record);
    }
}

bool DirectionStore::ValidateId(const std::string& id, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "direction id is required";
        return false;
    }
    if (id.size() > kIdMaxLength) {
        outError = "direction id must be at most 64 characters";
        return false;
    }
    if (id.compare(0, std::char_traits<char>::length(kIdPrefix), kIdPrefix) != 0) {
        outError = "direction id must start with 'dir_'";
        return false;
    }
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-')) {
            outError = "direction id may contain only letters, digits, '_' and '-'";
            return false;
        }
    }
    return true;
}

bool DirectionStore::ValidateCardNumber(const std::string& number, std::string& outError) {
    outError.clear();
    if (number.empty()) {
        outError = "card number is required";
        return false;
    }
    if (number.size() > kCardNumberMaxBytes) {
        outError = "card number must be at most 64 characters";
        return false;
    }
    if (number.compare(0, std::char_traits<char>::length(kIdPrefix), kIdPrefix) == 0) {
        outError = "card number must not start with 'dir_'";
        return false;
    }
    for (char c : number) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_' || c == '.')) {
            outError = "card number may contain only letters, digits, '-', '_' and '.'";
            return false;
        }
    }
    return true;
}

bool DirectionStore::ValidateSize(const char* field, const std::string& value, size_t maxBytes,
                                  std::string& outError) {
    outError.clear();
    if (value.size() <= maxBytes) return true;
    outError = std::string(field) + " exceeds " + std::to_string(maxBytes) + " bytes";
    return false;
}

std::string DirectionStore::NextId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    long long maxN = 0;
    for (const auto& [id, record] : records_) {
        (void)record;
        long long n = 0;
        if (ParseAutoNumber(id, n) && n > maxN) maxN = n;
    }
    return std::string(kIdPrefix) + std::to_string(maxN + 1);
}

bool DirectionStore::WriteRecordLocked(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    const std::string id = record.value("id", std::string());
    try {
        WriteFileAtomicWide(RecordPathWide(dir_, id), record.dump(2) + "\n");
    } catch (const std::exception& e) {
        outError = std::string("failed to write direction record: ") + e.what();
        return false;
    }
    return true;
}

bool DirectionStore::Create(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) {
        outError = "direction record needs an id";
        return false;
    }
    const std::string id = record["id"].get<std::string>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) > 0) {
        outError = "direction '" + id + "' already exists";
        return false;
    }
    if (!WriteRecordLocked(record, outError)) return false;
    records_[id] = record;
    ++version_;
    return true;
}

bool DirectionStore::Save(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) {
        outError = "direction record needs an id";
        return false;
    }
    const std::string id = record["id"].get<std::string>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) == 0) {
        outError = "direction '" + id + "' not found";
        return false;
    }
    if (!WriteRecordLocked(record, outError)) return false;
    records_[id] = record;
    ++version_;
    return true;
}

bool DirectionStore::Remove(const std::string& id, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) == 0) {
        outError = "direction '" + id + "' not found";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(RecordPathWide(dir_, id)), ec);
    records_.erase(id);
    ++version_;
    return true;
}

bool DirectionStore::Get(const std::string& id, nlohmann::json& outRecord) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(id);
    if (it == records_.end()) return false;
    outRecord = it->second;
    return true;
}

bool DirectionStore::Exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.count(id) > 0;
}

std::vector<nlohmann::json> DirectionStore::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    out.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        (void)id;
        out.push_back(record);
    }
    return out;
}

int64_t DirectionStore::Version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

uint64_t DirectionStore::MaxRev() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t maxRev = 0;
    for (const auto& [id, record] : records_) {
        (void)id;
        if (record.is_object() && record.contains("rev") && record["rev"].is_number_unsigned()) {
            const uint64_t r = record["rev"].get<uint64_t>();
            if (r > maxRev) maxRev = r;
        }
    }
    return maxRev;
}

std::vector<nlohmann::json> DirectionStore::SinceRev(uint64_t sinceRev, bool includeZero) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    for (const auto& [id, record] : records_) {
        (void)id;
        uint64_t r = 0;
        if (record.is_object() && record.contains("rev") && record["rev"].is_number_unsigned()) {
            r = record["rev"].get<uint64_t>();
        }
        if ((includeZero && r == 0) || r > sinceRev) {
            out.push_back(record);
        }
    }
    return out;
}

} // namespace pc
