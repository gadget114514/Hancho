#pragma once

#include "nlohmann/json.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pc {

// Task records (doc/task-lineage-plan.md, card 100-2a).
//
// A task is a Hancho record: its intent, its close decision and an append-only log of what
// happened (<data-dir>/tasks/<id>.json and <id>.log.jsonl). The authoritative state is still
// computed from directions/jobs; `state` is refreshed on state-change events and used for display
// and change notifications.
//
// Lock order: TaskQueueManager::mutex_ -> TaskStore::mutex_. TaskStore never calls back into
// the manager.
class TaskStore {
public:
    static constexpr size_t kTitleMaxBytes = 200;
    static constexpr size_t kIntentMaxBytes = 16 * 1024;
    static constexpr size_t kNoteMaxBytes = 4 * 1024;
    static constexpr size_t kReasonMaxBytes = 2000;
    static constexpr size_t kIdMaxLength = 64;
    static constexpr const char* kIdPrefix = "task_";

    // Creates dir when needed and loads every record found there.
    void Open(const std::wstring& dir);

    // An explicit id must start with "task_", be at most 64 characters and contain only ASCII
    // letters, digits, '_' and '-'.
    static bool ValidateId(const std::string& id, std::string& outError);
    // Fails when value is longer than maxBytes.
    static bool ValidateSize(const char* field, const std::string& value, size_t maxBytes,
                             std::string& outError);

    // The next automatic id: "task_<N>" where N is one more than the largest existing number.
    std::string NextId() const;

    // Stores a new record. record["id"] must be set; fails when the id already exists.
    bool Create(const nlohmann::json& record, std::string& outError);
    // Replaces an existing record and writes it to disk. Bumps Version().
    bool Save(const nlohmann::json& record, std::string& outError);
    // Deletes the record and its log. Used only by purge of closed tasks.
    bool Remove(const std::string& id, std::string& outError);

    bool Get(const std::string& id, nlohmann::json& outRecord) const;
    bool Exists(const std::string& id) const;
    // Snapshot of every record.
    std::vector<nlohmann::json> All() const;

    // Appends one line to the task's log. The entry is written as given (callers fill in "at").
    bool AppendLog(const std::string& id, const nlohmann::json& entry, std::string& outError);
    // The last `limit` log entries in order (limit 0 = all).
    std::vector<nlohmann::json> ReadLog(const std::string& id, size_t limit) const;

    // Counter that changes on every write, so the GUI can fetch only what changed.
    int64_t Version() const;
    // Maximum rev across all loaded records (0 when none).
    uint64_t MaxRev() const;
    // Snapshot of every record with rev > sinceRev (or all records when includeZero is true).
    std::vector<nlohmann::json> SinceRev(uint64_t sinceRev, bool includeZero = false) const;

private:
    bool WriteRecordLocked(const nlohmann::json& record, std::string& outError);

    std::wstring dir_;
    mutable std::mutex mutex_;
    std::map<std::string, nlohmann::json> records_;
    int64_t version_ = 0;
};

} // namespace pc
