#include "task_store.h"

#include "common.h"

#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>

namespace pc {

namespace {

std::wstring RecordPathWide(const std::wstring& dir, const std::string& id) {
    return PathJoinWide(dir, Utf8ToWide(id + ".json"));
}

std::wstring LogPathWide(const std::wstring& dir, const std::string& id) {
    return PathJoinWide(dir, Utf8ToWide(id + ".log.jsonl"));
}

bool ParseAutoNumber(const std::string& id, long long& outN) {
    const std::string prefix = TaskStore::kIdPrefix;
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

void TaskStore::Open(const std::wstring& dir) {
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
        // Log files end in ".log.jsonl" and have the extension ".jsonl", so they are skipped here.
        if (p.extension() != ".json") continue;
        const std::string id = p.stem().string();
        nlohmann::json record = nlohmann::json::parse(ReadFileUtf8Wide(p.wstring()), nullptr, false);
        if (record.is_discarded() || !record.is_object()) continue;
        if (!record.contains("id") || !record["id"].is_string()) continue;
        records_[id] = std::move(record);
    }
}

bool TaskStore::ValidateId(const std::string& id, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "task id is required";
        return false;
    }
    if (id.size() > kIdMaxLength) {
        outError = "task id must be at most 64 characters";
        return false;
    }
    if (id.compare(0, std::char_traits<char>::length(kIdPrefix), kIdPrefix) != 0) {
        outError = "task id must start with 'task_'";
        return false;
    }
    for (char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-')) {
            outError = "task id may contain only letters, digits, '_' and '-'";
            return false;
        }
    }
    return true;
}

bool TaskStore::ValidateSize(const char* field, const std::string& value, size_t maxBytes,
                             std::string& outError) {
    outError.clear();
    if (value.size() <= maxBytes) return true;
    outError = std::string(field) + " exceeds " + std::to_string(maxBytes) + " bytes";
    return false;
}

std::string TaskStore::NextId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    long long maxN = 0;
    for (const auto& [id, record] : records_) {
        (void)record;
        long long n = 0;
        if (ParseAutoNumber(id, n) && n > maxN) maxN = n;
    }
    return std::string(kIdPrefix) + std::to_string(maxN + 1);
}

bool TaskStore::WriteRecordLocked(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    const std::string id = record.value("id", std::string());
    try {
        WriteFileAtomicWide(RecordPathWide(dir_, id), record.dump(2) + "\n");
    } catch (const std::exception& e) {
        outError = std::string("failed to write task record: ") + e.what();
        return false;
    }
    return true;
}

bool TaskStore::Create(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) {
        outError = "task record needs an id";
        return false;
    }
    const std::string id = record["id"].get<std::string>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) > 0) {
        outError = "task '" + id + "' already exists";
        return false;
    }
    if (!WriteRecordLocked(record, outError)) return false;
    records_[id] = record;
    ++version_;
    return true;
}

bool TaskStore::Save(const nlohmann::json& record, std::string& outError) {
    outError.clear();
    if (!record.is_object() || !record.contains("id") || !record["id"].is_string()) {
        outError = "task record needs an id";
        return false;
    }
    const std::string id = record["id"].get<std::string>();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) == 0) {
        outError = "task '" + id + "' not found";
        return false;
    }
    if (!WriteRecordLocked(record, outError)) return false;
    records_[id] = record;
    ++version_;
    return true;
}

bool TaskStore::Remove(const std::string& id, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) == 0) {
        outError = "task '" + id + "' not found";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(RecordPathWide(dir_, id)), ec);
    std::filesystem::remove(std::filesystem::path(LogPathWide(dir_, id)), ec);
    records_.erase(id);
    ++version_;
    return true;
}

bool TaskStore::Get(const std::string& id, nlohmann::json& outRecord) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(id);
    if (it == records_.end()) return false;
    outRecord = it->second;
    return true;
}

bool TaskStore::Exists(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.count(id) > 0;
}

std::vector<nlohmann::json> TaskStore::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> out;
    out.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        (void)id;
        out.push_back(record);
    }
    return out;
}

bool TaskStore::AppendLog(const std::string& id, const nlohmann::json& entry, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.count(id) == 0) {
        outError = "task '" + id + "' not found";
        return false;
    }
    try {
        std::ofstream out(std::filesystem::path(LogPathWide(dir_, id)), std::ios::app | std::ios::binary);
        if (!out) {
            outError = "failed to open task log for '" + id + "'";
            return false;
        }
        out << entry.dump() << "\n";
        if (!out) {
            outError = "failed to write task log for '" + id + "'";
            return false;
        }
    } catch (const std::exception& e) {
        outError = std::string("failed to write task log: ") + e.what();
        return false;
    }
    ++version_;
    return true;
}

std::vector<nlohmann::json> TaskStore::ReadLog(const std::string& id, size_t limit) const {
    std::vector<nlohmann::json> entries;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = LogPathWide(dir_, id);
    }
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return entries;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        nlohmann::json entry = nlohmann::json::parse(line, nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;
        entries.push_back(std::move(entry));
    }
    if (limit > 0 && entries.size() > limit) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return entries;
}

int64_t TaskStore::Version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

} // namespace pc
