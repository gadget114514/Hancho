#pragma once

#include "nlohmann/json.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pc {

class ProcessManager;
class WorkerEngineRegistry;

// Whether a text of the given kind ("card.title", "card.body", or "" for any other text) is
// translated under the config. translate_mode "off" turns translation off for every kind;
// translate_cards "title" translates card titles only and "off" translates no card text.
bool ShouldTranslate(const std::string& field, const nlohmann::json& config);

// Parses the reply of a translate-llm run: a JSON array of exactly expectedCount non-empty
// strings, optionally wrapped in text or a code fence.
bool ParseTranslationReply(const std::string& reply, size_t expectedCount,
                           std::vector<std::string>& out, std::string& err);

// Translation cache and worker (card 106-2). Stored data is English; translations for the
// display languages are kept in <data-dir>/translations/<lang>.json, keyed by the SHA-256 of the
// original UTF-8 text, with the original text checked on every lookup.
class TranslationStore {
public:
    using EnginesProvider = std::function<const WorkerEngineRegistry*()>;
    // Translates sources (English) into lang. Returns true with one translation per source.
    using BatchRunner = std::function<bool(const std::string& lang, const std::string& engine,
                                           const std::string& model,
                                           const std::vector<std::string>& sources,
                                           std::vector<std::string>& outTranslations,
                                           std::string& outError)>;

    // pm may be null when a batch runner is set (tests). engines may return null, in which
    // case <data-dir>/engines.json is read.
    TranslationStore(ProcessManager* pm, EnginesProvider engines);
    ~TranslationStore();

    TranslationStore(const TranslationStore&) = delete;
    TranslationStore& operator=(const TranslationStore&) = delete;

    void Start();
    void Stop();

    // The translation of text for lang, or the text itself when no translation applies (English,
    // empty, not translated by config, or already in Japanese for ja). nullopt when a translation
    // is wanted but not cached yet.
    std::optional<std::string> Lookup(const std::string& text, const std::string& lang,
                                      const std::string& field = "");
    // Queues the texts that have no cached translation and are not queued, running or waiting
    // after a failure. Returns the number queued by this call.
    size_t Request(const std::vector<std::string>& texts, const std::string& lang,
                   const std::string& field = "");
    // Foreign-to-English (card 106-5). foreign: the caller found the text is not English
    // (see IsForeignText). LookupEnglish returns the cached English text, the text itself when
    // no translation applies, nullopt when one is wanted but not cached. RequestEnglish queues
    // a foreign text for an English translation (the same queue, dedupe and failure rules).
    std::optional<std::string> LookupEnglish(const std::string& text, bool foreign,
                                             const std::string& field = "");
    size_t RequestEnglish(const std::string& text, bool foreign, const std::string& field = "");
    // True when translations can run now for field (engine set, translate_mode on, and the field
    // is not excluded by translate_cards).
    bool CanTranslate(const std::string& field);
    // Counter that goes up whenever a translation lands or the cache is cleared.
    uint64_t Revision();

    // JSON reply for translate_get: one item per text with its state (translated, original,
    // pending, failed, unavailable). Missing texts are queued.
    std::string Get(const std::vector<std::string>& texts, const std::string& lang,
                    const std::string& field = "");
    // translate_get with lang "en" (card 106-5): the texts are non-English originals. Returns each
    // one's English translation when cached; otherwise the state, and a missing one is queued.
    std::string GetEnglish(const std::vector<std::string>& texts, const std::string& field = "");
    // JSON reply for translate_status.
    std::string Status();
    // Drops cached translations and failure records for lang (all languages when empty).
    std::string Clear(const std::string& lang = "");

    // Reads config.json (translate_* keys, falling back to triage_llm_engine / triage_llm_model).
    void ReloadConfig();
    // Applies a config object. Changing the engine, model, mode or card setting drops the queue;
    // cached translations are kept.
    void ApplyConfig(const nlohmann::json& config);

    // Test hooks. RunOneBatch runs one batch on the calling thread and returns false when there
    // was nothing to run.
    void SetBatchRunner(BatchRunner runner);
    void SetMaxEntries(size_t maxEntries);
    bool RunOneBatch();

private:
    struct Entry {
        std::string source;
        std::string translated;
        std::string engine;
        std::string model;
        std::string createdAt;
        uint64_t lastUsed = 0;
    };
    struct Failure {
        std::string source;
        std::string error;
        int count = 0;
        int64_t retryAtEpoch = 0;
    };
    struct LangState {
        bool loaded = false;
        bool dirty = false;
        std::unordered_map<std::string, Entry> entries;
        std::unordered_map<std::string, Failure> failures;
    };
    struct Item {
        std::string lang;
        std::string hash;
        std::string text;
    };

    bool CanRunLocked() const;
    std::optional<std::string> LookupLocked(const std::string& text, const std::string& lang,
                                            const std::string& field);
    size_t RequestLocked(const std::vector<std::string>& texts, const std::string& lang,
                         const std::string& field, std::vector<std::string>* queued);
    bool FailedLocked(LangState& st, const std::string& text, const std::string& hash) const;
    LangState& LoadLocked(const std::string& lang);
    void EvictLocked();
    void SaveDirtyLocked(bool force);
    void TakeBatchLocked(std::vector<Item>& batch, std::string& lang, std::string& engine,
                         std::string& model);
    void ApplyBatchLocked(const std::vector<Item>& batch, bool ok,
                          const std::vector<std::string>& translated, const std::string& engine,
                          const std::string& model, const std::string& error);
    bool RunProcessBatch(const std::string& lang, const std::string& engine, const std::string& model,
                         const std::vector<std::string>& sources, std::vector<std::string>& out,
                         std::string& err);
    void WorkerLoop();

    ProcessManager* pm_ = nullptr;
    EnginesProvider enginesProvider_;
    BatchRunner runner_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{true};

    nlohmann::json config_ = nlohmann::json::object();
    std::string mode_ = "auto";
    std::string engine_;
    std::string model_;
    std::string cards_ = "all";

    std::map<std::string, LangState> langs_;
    std::deque<Item> queue_;
    std::set<std::string> queuedKeys_;
    std::set<std::string> inFlight_;
    size_t maxEntries_ = 20000;
    uint64_t useSeq_ = 0;
    uint64_t revision_ = 0;
    uint64_t batchSeq_ = 0;
    uint64_t lastSaveMs_ = 0;
    std::string lastError_;
};

// The store the IPC, MCP and CLI front ends use. Set by the core host at start-up.
TranslationStore* ActiveTranslationStore();
void SetActiveTranslationStore(TranslationStore* store);

// Card 106-5. A text is foreign when the caller's sourceLang is not "en" (when given), or, without
// a sourceLang, when IsNonEnglishText says so.
bool IsForeignText(const std::string& text, const std::string& sourceLang);
// Language tag of a foreign text with no sourceLang: "ja" (kana or kanji), "ko" (hangul), else "und".
std::string DetectTextLang(const std::string& text);
// Moves a foreign <field> of obj to English. The original is kept in <field>Lang. When the English
// translation is cached, <field> becomes the English text, <field>Source becomes {lang, text} and
// <field>Lang becomes "en". Otherwise the translation is queued (cardField names the card setting
// that applies, "" for other text), and false is returned with the original kept. Returns true when
// <field> is English now. Calling it again later applies a translation that has landed since.
// store may be null, in which case the original is only tagged.
bool NormalizeEnglishField(nlohmann::json& obj, const std::string& field, const std::string& sourceLang,
                           const std::string& cardField, TranslationStore* store);
// The text to show for <field> in displayLang: the original when <field>Source.lang is displayLang,
// otherwise the field itself.
std::string EnglishFieldForDisplay(const nlohmann::json& obj, const std::string& field,
                                   const std::string& displayLang);

} // namespace pc
