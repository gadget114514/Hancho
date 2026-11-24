#pragma once

#include "nlohmann/json.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pc {

// Direction records (doc/terminology.md, doc/direction-cards.md card 103-1).
//
// A direction is one registration on the queue: either one behavior tree or one `taskqueue add`
// call (<data-dir>/directions/<id>.json). Its execution state is computed from its jobs / BT and
// is never stored here.
//
// Lock order: TaskQueueManager::mutex_ -> DirectionStore::mutex_ -> TaskStore::mutex_.
// DirectionStore never calls back into TaskQueueManager.
class DirectionStore {
public:
    static constexpr size_t kTitleMaxBytes = 200;
    static constexpr size_t kReasonMaxBytes = 2000;
    static constexpr size_t kIdMaxLength = 64;
    static constexpr const char* kIdPrefix = "dir_";
    static constexpr size_t kCardNumberMaxBytes = 64;
    static constexpr size_t kCardBodyMaxBytes = 65536;
    static constexpr size_t kCardTemplateMaxBytes = 64;

    // Creates dir when needed and loads every record found there.
    void Open(const std::wstring& dir);

    // An explicit id must start with "dir_", be at most 64 characters and contain only ASCII
    // letters, digits, '_' and '-'.
    static bool ValidateId(const std::string& id, std::string& outError);
    // A card number must be non-empty, at most 64 characters, contain only ASCII letters,
    // digits, '-', '_' and '.', and must not start with "dir_".
    static bool ValidateCardNumber(const std::string& number, std::string& outError);
    // Fails when value is longer than maxBytes.
    static bool ValidateSize(const char* field, const std::string& value, size_t maxBytes,
                             std::string& outError);

    // The next automatic id: "dir_<N>" where N is one more than the largest existing number.
    std::string NextId() const;

    // Stores a new record. record["id"] must be set; fails when the id already exists.
    bool Create(const nlohmann::json& record, std::string& outError);
    // Replaces an existing record and writes it to disk. Bumps Version().
    bool Save(const nlohmann::json& record, std::string& outError);
    // Deletes the record from disk and memory. Used by purge.
    bool Remove(const std::string& id, std::string& outError);

    bool Get(const std::string& id, nlohmann::json& outRecord) const;
    bool Exists(const std::string& id) const;
    // Snapshot of every record.
    std::vector<nlohmann::json> All() const;

    // Counter that changes on every write.
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
