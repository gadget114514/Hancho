#include "translation_store.h"

#include "common.h"
#include "process_manager.h"
#include "worker_engines.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace pc {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxBatchItems = 50;
constexpr size_t kMaxBatchChars = 8000;
constexpr int kFailureLimit = 3;
constexpr int64_t kRetryBackoffSec = 3600;
constexpr int kProcessTimeoutSec = 300;
constexpr uint64_t kSaveIntervalMs = 5000;
const char* const kStoppedMessage = "translation stopped";

std::atomic<TranslationStore*> g_activeStore{nullptr};

std::string CfgString(const json& cfg, const char* key, const std::string& fallback = "") {
    if (!cfg.is_object()) return fallback;
    auto it = cfg.find(key);
    if (it == cfg.end() || !it->is_string()) return fallback;
    std::string value = it->get<std::string>();
    return value.empty() ? fallback : value;
}

// Language codes are short lowercase tags (ja, es, pt-br). Anything else is refused, which also
// keeps the code safe to use in a file name.
bool IsValidLang(const std::string& lang) {
    if (lang.size() < 2 || lang.size() > 8) return false;
    for (char c : lang) {
        if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
    }
    return true;
}

std::wstring TranslationsDirectory() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"translations");
}

std::wstring LangFilePath(const std::string& lang) {
    return PathJoinWide(TranslationsDirectory(), Utf8ToWide(SanitizeFileNameComponent(lang)) + L".json");
}

std::string KeyOf(const std::string& lang, const std::string& hash) {
    return lang + "\n" + hash;
}

std::string DumpUtf8(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace

bool ShouldTranslate(const std::string& field, const json& config) {
    if (CfgString(config, "translate_mode", "off") == "off") return false;
    const std::string cards = CfgString(config, "translate_cards", "off");
    if (field == "card.title") return cards != "off";
    if (field == "card.body") return cards == "all";
    return true;
}

bool IsForeignText(const std::string& text, const std::string& sourceLang) {
    if (!sourceLang.empty()) return sourceLang != "en";
    return IsNonEnglishText(text);
}

std::string DetectTextLang(const std::string& text) {
    bool han = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + len > text.size()) break;
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
        for (size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        i += len;
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9D)) {
            return "ja";
        }
        if ((cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xAC00 && cp <= 0xD7AF)) return "ko";
        if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
            han = true;  // kanji alone: Japanese in practice
        }
    }
    return han ? "ja" : "und";
}

bool NormalizeEnglishField(json& obj, const std::string& field, const std::string& sourceLang,
                           const std::string& cardField, TranslationStore* store) {
    if (!obj.is_object() || !obj.contains(field) || !obj[field].is_string()) return true;
    if (obj.contains(field + "Source")) return true;  // already translated
    const std::string text = obj[field].get<std::string>();
    if (text.empty() || !IsForeignText(text, sourceLang)) return true;
    const std::string lang = (sourceLang.empty() || sourceLang == "en") ? DetectTextLang(text) : sourceLang;
    obj[field + "Lang"] = lang;
    if (!store || !store->CanTranslate(cardField)) return false;
    const std::optional<std::string> english = store->LookupEnglish(text, true, cardField);
    if (english && *english != text) {
        obj[field + "Source"] = {{"lang", lang}, {"text", text}};
        obj[field] = *english;
        obj[field + "Lang"] = "en";
        return true;
    }
    store->RequestEnglish(text, true, cardField);
    return false;
}

std::string EnglishFieldForDisplay(const json& obj, const std::string& field, const std::string& displayLang) {
    if (!obj.is_object()) return std::string();
    const std::string value = obj.value(field, std::string());
    auto source = obj.find(field + "Source");
    if (source != obj.end() && source->is_object() && source->value("lang", std::string()) == displayLang) {
        return source->value("text", value);
    }
    return value;
}

bool ParseTranslationReply(const std::string& reply, size_t expectedCount,
                           std::vector<std::string>& out, std::string& err) {
    const size_t open = reply.find('[');
    const size_t close = reply.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        err = "the reply is not a JSON array";
        return false;
    }
    json parsed = json::parse(reply.substr(open, close - open + 1), nullptr, false);
    if (!parsed.is_array()) {
        err = "the reply is not a JSON array";
        return false;
    }
    if (parsed.size() != expectedCount) {
        err = "the reply length does not match the batch";
        return false;
    }
    std::vector<std::string> translations;
    translations.reserve(parsed.size());
    for (const auto& item : parsed) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            err = "the reply has an empty or non-string item";
            return false;
        }
        translations.push_back(item.get<std::string>());
    }
    out = std::move(translations);
    return true;
}

TranslationStore::TranslationStore(ProcessManager* pm, EnginesProvider engines)
    : pm_(pm), enginesProvider_(std::move(engines)) {}

TranslationStore::~TranslationStore() {
    Stop();
}

void TranslationStore::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable()) return;
        stop_ = false;
    }
    ReloadConfig();
    worker_ = std::thread(&TranslationStore::WorkerLoop, this);
}

void TranslationStore::Stop() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    SaveDirtyLocked(true);
}

bool TranslationStore::CanRunLocked() const {
    return mode_ != "off" && !engine_.empty();
}

std::optional<std::string> TranslationStore::LookupLocked(const std::string& text, const std::string& lang,
                                                          const std::string& field) {
    if (text.empty() || !IsValidLang(lang) || lang == "en") return text;
    if (!ShouldTranslate(field, config_)) return text;
    if (lang == "ja" && IsNonEnglishText(text)) return text;
    LangState& st = LoadLocked(lang);
    auto it = st.entries.find(Sha256Hex(text));
    if (it == st.entries.end() || it->second.source != text) return std::nullopt;
    it->second.lastUsed = ++useSeq_;
    return it->second.translated;
}

std::optional<std::string> TranslationStore::Lookup(const std::string& text, const std::string& lang,
                                                    const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LookupLocked(text, lang, field);
}

bool TranslationStore::FailedLocked(LangState& st, const std::string& text, const std::string& hash) const {
    auto it = st.failures.find(hash);
    if (it == st.failures.end() || it->second.source != text) return false;
    if (it->second.count >= kFailureLimit) return true;
    return it->second.retryAtEpoch > GetNowEpochSec();
}

size_t TranslationStore::RequestLocked(const std::vector<std::string>& texts, const std::string& lang,
                                       const std::string& field, std::vector<std::string>* queued) {
    if (!IsValidLang(lang) || lang == "en" || !CanRunLocked() || !ShouldTranslate(field, config_)) return 0;
    LangState& st = LoadLocked(lang);
    size_t count = 0;
    for (const auto& text : texts) {
        if (text.empty() || (lang == "ja" && IsNonEnglishText(text))) continue;
        const std::string hash = Sha256Hex(text);
        auto entry = st.entries.find(hash);
        if (entry != st.entries.end() && entry->second.source == text) continue;
        if (FailedLocked(st, text, hash)) continue;
        const std::string key = KeyOf(lang, hash);
        if (queuedKeys_.count(key) || inFlight_.count(key)) continue;
        queue_.push_back(Item{lang, hash, text});
        queuedKeys_.insert(key);
        ++count;
        if (queued) queued->push_back(text);
    }
    return count;
}

size_t TranslationStore::Request(const std::vector<std::string>& texts, const std::string& lang,
                                 const std::string& field) {
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = RequestLocked(texts, lang, field, nullptr);
    }
    if (count) cv_.notify_all();
    return count;
}

bool TranslationStore::CanTranslate(const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    return CanRunLocked() && ShouldTranslate(field, config_);
}

std::optional<std::string> TranslationStore::LookupEnglish(const std::string& text, bool foreign,
                                                           const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (text.empty() || !foreign || !ShouldTranslate(field, config_)) return text;
    LangState& st = LoadLocked("en");
    auto it = st.entries.find(Sha256Hex(text));
    if (it == st.entries.end() || it->second.source != text) return std::nullopt;
    it->second.lastUsed = ++useSeq_;
    return it->second.translated;
}

size_t TranslationStore::RequestEnglish(const std::string& text, bool foreign, const std::string& field) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text.empty() || !foreign || !CanRunLocked() || !ShouldTranslate(field, config_)) return 0;
        LangState& st = LoadLocked("en");
        const std::string hash = Sha256Hex(text);
        auto entry = st.entries.find(hash);
        if (entry != st.entries.end() && entry->second.source == text) return 0;
        if (FailedLocked(st, text, hash)) return 0;
        const std::string key = KeyOf("en", hash);
        if (queuedKeys_.count(key) || inFlight_.count(key)) return 0;
        queue_.push_back(Item{"en", hash, text});
        queuedKeys_.insert(key);
    }
    cv_.notify_all();
    return 1;
}

uint64_t TranslationStore::Revision() {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::string TranslationStore::Get(const std::vector<std::string>& texts, const std::string& lang,
                                  const std::string& field) {
    if (lang == "en") return GetEnglish(texts, field);
    if (!IsValidLang(lang)) return json{{"ok", false}, {"error", "unknown language: " + lang}}.dump();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> queued;
    RequestLocked(texts, lang, field, &queued);
    LangState& st = LoadLocked(lang);
    json items = json::array();
    for (const auto& text : texts) {
        json item = {{"text", text}, {"translated", nullptr}};
        auto found = LookupLocked(text, lang, field);
        if (found && *found != text) {
            item["translated"] = *found;
            item["state"] = "translated";
        } else if (found) {
            item["state"] = "original";
        } else if (!CanRunLocked()) {
            item["state"] = "unavailable";
        } else if (FailedLocked(st, text, Sha256Hex(text))) {
            item["state"] = "failed";
        } else {
            item["state"] = "pending";
        }
        items.push_back(item);
    }
    if (!queued.empty()) cv_.notify_all();
    return json{{"ok", true}, {"lang", lang}, {"revision", revision_},
                {"enabled", CanRunLocked()}, {"items", items}}.dump();
}

std::string TranslationStore::GetEnglish(const std::vector<std::string>& texts, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    LangState& st = LoadLocked("en");
    const bool wanted = ShouldTranslate(field, config_);
    bool queuedAny = false;
    json items = json::array();
    for (const auto& text : texts) {
        json item = {{"text", text}, {"translated", nullptr}};
        const std::string hash = Sha256Hex(text);
        if (text.empty() || !IsNonEnglishText(text) || !wanted) {
            item["state"] = "original";
        } else {
            auto it = st.entries.find(hash);
            if (it != st.entries.end() && it->second.source == text) {
                it->second.lastUsed = ++useSeq_;
                item["translated"] = it->second.translated;
                item["state"] = "translated";
            } else if (!CanRunLocked()) {
                item["state"] = "unavailable";
            } else if (FailedLocked(st, text, hash)) {
                item["state"] = "failed";
            } else {
                item["state"] = "pending";
                const std::string key = KeyOf("en", hash);
                if (!queuedKeys_.count(key) && !inFlight_.count(key)) {
                    queue_.push_back(Item{"en", hash, text});
                    queuedKeys_.insert(key);
                    queuedAny = true;
                }
            }
        }
        items.push_back(item);
    }
    if (queuedAny) cv_.notify_all();
    return json{{"ok", true}, {"lang", "en"}, {"revision", revision_},
                {"enabled", CanRunLocked()}, {"items", items}}.dump();
}

std::string TranslationStore::Status() {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadLocked("ja");
    LoadLocked("es");
    json cached = json::object();
    size_t failedPermanent = 0;
    for (const auto& [lang, st] : langs_) {
        cached[lang] = st.entries.size();
        for (const auto& [hash, failure] : st.failures) {
            if (failure.count >= kFailureLimit) ++failedPermanent;
        }
    }
    return json{{"ok", true}, {"revision", revision_}, {"mode", mode_}, {"enabled", CanRunLocked()},
                {"engine", engine_}, {"model", model_}, {"cards", cards_}, {"cached", cached},
                {"queued", queue_.size()}, {"inFlight", inFlight_.size()},
                {"failedPermanent", failedPermanent}, {"lastError", lastError_},
                {"workerRunning", !stop_.load()}, {"maxEntries", maxEntries_}}.dump();
}

std::string TranslationStore::Clear(const std::string& lang) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lang.empty() && !IsValidLang(lang)) return json{{"ok", false}, {"error", "unknown language: " + lang}}.dump();
    std::vector<std::string> targets;
    if (lang.empty()) {
        LoadLocked("ja");
        LoadLocked("es");
        for (const auto& [name, st] : langs_) targets.push_back(name);
    } else {
        LoadLocked(lang);
        targets.push_back(lang);
    }
    size_t removed = 0;
    for (const auto& target : targets) {
        LangState& st = LoadLocked(target);
        removed += st.entries.size();
        st.entries.clear();
        st.failures.clear();
        st.dirty = true;
        const std::string prefix = target + "\n";
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                    [&target](const Item& item) { return item.lang == target; }),
                     queue_.end());
        for (auto it = queuedKeys_.begin(); it != queuedKeys_.end();) {
            it = it->rfind(prefix, 0) == 0 ? queuedKeys_.erase(it) : std::next(it);
        }
    }
    ++revision_;
    SaveDirtyLocked(true);
    return json{{"ok", true}, {"cleared", removed}, {"revision", revision_}}.dump();
}

void TranslationStore::ReloadConfig() {
    json config = json::parse(ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json")),
                              nullptr, false);
    ApplyConfig(config.is_object() ? config : json::object());
}

void TranslationStore::ApplyConfig(const json& config) {
    std::string mode = CfgString(config, "translate_mode", "off");
    if (mode != "auto" && mode != "off") mode = "off";
    std::string engine = CfgString(config, "translate_engine");
    if (engine.empty()) engine = CfgString(config, "triage_llm_engine");
    std::string model = CfgString(config, "translate_model");
    if (model.empty()) model = CfgString(config, "triage_llm_model");
    std::string cards = CfgString(config, "translate_cards", "off");
    if (cards != "all" && cards != "title" && cards != "off") cards = "off";

    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = mode != mode_ || engine != engine_ || model != model_ || cards != cards_;
    config_ = config;
    mode_ = mode;
    engine_ = engine;
    model_ = model;
    cards_ = cards;
    if (changed) {
        // Queued work was asked for under the old engine or card setting. Cached translations stay.
        queue_.clear();
        queuedKeys_.clear();
    }
}

void TranslationStore::SetBatchRunner(BatchRunner runner) {
    std::lock_guard<std::mutex> lock(mutex_);
    runner_ = std::move(runner);
}

void TranslationStore::SetMaxEntries(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxEntries_ = maxEntries;
    EvictLocked();
}

TranslationStore::LangState& TranslationStore::LoadLocked(const std::string& lang) {
    LangState& st = langs_[lang];
    if (st.loaded) return st;
    st.loaded = true;
    if (!IsValidLang(lang)) return st;
    json doc = json::parse(ReadFileUtf8Wide(LangFilePath(lang)), nullptr, false);
    if (!doc.is_object()) return st;
    if (doc.contains("entries") && doc["entries"].is_object()) {
        const json& entries = doc["entries"];
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const json& value = it.value();
            if (!value.is_object()) continue;
            Entry entry;
            entry.source = value.value("source", std::string());
            entry.translated = value.value("translated", std::string());
            entry.engine = value.value("engine", std::string());
            entry.model = value.value("model", std::string());
            entry.createdAt = value.value("createdAt", std::string());
            entry.lastUsed = value.value("lastUsed", uint64_t{0});
            if (entry.source.empty() || entry.translated.empty()) continue;
            useSeq_ = std::max(useSeq_, entry.lastUsed);
            st.entries.emplace(it.key(), std::move(entry));
        }
    }
    if (doc.contains("failed") && doc["failed"].is_object()) {
        const json& failed = doc["failed"];
        for (auto it = failed.begin(); it != failed.end(); ++it) {
            const json& value = it.value();
            if (!value.is_object()) continue;
            Failure failure;
            failure.source = value.value("source", std::string());
            failure.error = value.value("error", std::string());
            failure.count = value.value("count", 0);
            failure.retryAtEpoch = value.value("retryAtEpoch", int64_t{0});
            if (failure.source.empty()) continue;
            st.failures.emplace(it.key(), std::move(failure));
        }
    }
    return st;
}

void TranslationStore::EvictLocked() {
    size_t total = 0;
    for (const auto& [lang, st] : langs_) total += st.entries.size();
    while (total > maxEntries_) {
        LangState* victimLang = nullptr;
        std::string victimHash;
        uint64_t oldest = (std::numeric_limits<uint64_t>::max)();
        for (auto& [lang, st] : langs_) {
            for (const auto& [hash, entry] : st.entries) {
                if (entry.lastUsed < oldest) {
                    oldest = entry.lastUsed;
                    victimLang = &st;
                    victimHash = hash;
                }
            }
        }
        if (!victimLang) break;
        victimLang->entries.erase(victimHash);
        victimLang->dirty = true;
        --total;
    }
}

void TranslationStore::SaveDirtyLocked(bool force) {
    const uint64_t now = GetNowTickMs();
    if (!force && now - lastSaveMs_ < kSaveIntervalMs) return;
    bool any = false;
    for (auto& [lang, st] : langs_) {
        if (!st.dirty) continue;
        if (!any) {
            EnsureDirectoryWide(TranslationsDirectory());
            any = true;
        }
        json entries = json::object();
        for (const auto& [hash, entry] : st.entries) {
            entries[hash] = {{"source", entry.source}, {"translated", entry.translated},
                             {"engine", entry.engine}, {"model", entry.model},
                             {"createdAt", entry.createdAt}, {"lastUsed", entry.lastUsed}};
        }
        json failed = json::object();
        for (const auto& [hash, failure] : st.failures) {
            failed[hash] = {{"source", failure.source}, {"error", failure.error},
                            {"count", failure.count}, {"retryAtEpoch", failure.retryAtEpoch}};
        }
        json doc = {{"version", 1}, {"lang", lang}, {"entries", entries}, {"failed", failed}};
        WriteFileAtomicWide(LangFilePath(lang), DumpUtf8(doc));
        st.dirty = false;
    }
    if (any) lastSaveMs_ = now;
}

void TranslationStore::TakeBatchLocked(std::vector<Item>& batch, std::string& lang, std::string& engine,
                                       std::string& model) {
    lang = queue_.front().lang;
    engine = engine_;
    model = model_;
    size_t chars = 0;
    while (!queue_.empty() && batch.size() < kMaxBatchItems && queue_.front().lang == lang) {
        const size_t length = queue_.front().text.size();
        if (!batch.empty() && chars + length > kMaxBatchChars) break;
        chars += length;
        Item item = std::move(queue_.front());
        queue_.pop_front();
        const std::string key = KeyOf(item.lang, item.hash);
        queuedKeys_.erase(key);
        inFlight_.insert(key);
        batch.push_back(std::move(item));
    }
}

void TranslationStore::ApplyBatchLocked(const std::vector<Item>& batch, bool ok,
                                        const std::vector<std::string>& translated, const std::string& engine,
                                        const std::string& model, const std::string& error) {
    const int64_t now = GetNowEpochSec();
    const bool stopped = error == kStoppedMessage;
    for (size_t i = 0; i < batch.size(); ++i) {
        const Item& item = batch[i];
        inFlight_.erase(KeyOf(item.lang, item.hash));
        LangState& st = LoadLocked(item.lang);
        if (ok) {
            Entry& entry = st.entries[item.hash];
            entry.source = item.text;
            entry.translated = translated[i];
            entry.engine = engine;
            entry.model = model;
            entry.createdAt = GetCurrentTimestampStr();
            entry.lastUsed = ++useSeq_;
            st.failures.erase(item.hash);
        } else if (!stopped) {
            // A stop is not the engine's fault, so it does not count toward the failure limit.
            Failure& failure = st.failures[item.hash];
            if (failure.source != item.text) failure = Failure{};
            failure.source = item.text;
            failure.error = error;
            failure.count += 1;
            failure.retryAtEpoch = now + kRetryBackoffSec;
        }
        st.dirty = true;
    }
    if (ok) {
        ++revision_;
        EvictLocked();
    } else {
        lastError_ = error;
    }
}

bool TranslationStore::RunOneBatch() {
    std::vector<Item> batch;
    std::string lang, engine, model;
    BatchRunner runner;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!CanRunLocked() || queue_.empty()) return false;
        TakeBatchLocked(batch, lang, engine, model);
        runner = runner_;
    }
    std::vector<std::string> sources;
    sources.reserve(batch.size());
    for (const auto& item : batch) sources.push_back(item.text);

    std::vector<std::string> translated;
    std::string error;
    const bool ok = runner ? runner(lang, engine, model, sources, translated, error)
                           : RunProcessBatch(lang, engine, model, sources, translated, error);
    std::lock_guard<std::mutex> lock(mutex_);
    ApplyBatchLocked(batch, ok, translated, engine, model, error);
    return true;
}

bool TranslationStore::RunProcessBatch(const std::string& lang, const std::string& engine,
                                       const std::string& model, const std::vector<std::string>& sources,
                                       std::vector<std::string>& out, std::string& err) {
    if (!pm_) {
        err = "process manager is unavailable";
        return false;
    }
    WorkerEngineRegistry localEngines;
    const WorkerEngineRegistry* engines = enginesProvider_ ? enginesProvider_() : nullptr;
    if (!engines) {
        const std::string engineConfig = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"engines.json"));
        if (!engineConfig.empty()) {
            std::string loadError;
            if (!localEngines.LoadFromJson(engineConfig, loadError)) {
                err = loadError;
                return false;
            }
        }
        engines = &localEngines;
    }
    const std::string selected = engine.empty() ? engines->DefaultEngine() : engine;
    if (selected.empty()) {
        err = "no worker engine is configured";
        return false;
    }
    const WorkerEngineDef* def = engines->Find(selected);
    if (!def) {
        err = "unknown worker engine: " + selected;
        return false;
    }

    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = ++batchSeq_;
    }
    const std::string batchId = "translate-" + std::to_string(GetNowTickMs()) + "-" + std::to_string(seq);
    const std::wstring workDir = PathJoinWide(TranslationsDirectory(), L"work");
    EnsureDirectoryWide(workDir);
    const std::wstring inputPath = PathJoinWide(workDir, Utf8ToWide(SanitizeFileNameComponent(batchId)) + L".json");
    WriteFileAtomicWide(inputPath, DumpUtf8(json(sources)));

    const bool toEnglish = lang == "en";
    const std::string targetName = toEnglish ? "English" : lang == "ja" ? "Japanese" : lang == "es" ? "Spanish" : lang;
    const std::string sourceName = toEnglish ? "non-English strings" : "English strings";
    const std::string prompt = "Read the file " + WideToUtf8(inputPath) +
        ". It holds a JSON array of " + sourceName + ". Translate each string into " + targetName +
        ". Reply with only a JSON array of the same length and in the same order as your final message. "
        "Keep identifiers, paths, commands, ${...} variables, numbers and backtick code spans exactly as they are. "
        "Do not change any file.";
    std::string command, streamType, commandError, adapter;
    if (!engines->BuildCommand(selected, model, prompt, kProcessTimeoutSec, command, streamType,
                               commandError, &adapter, &def->advicePermissionArgs)) {
        err = commandError;
        RemoveFileWide(inputPath);
        return false;
    }

    const std::wstring logDir = PathJoinWide(TranslationsDirectory(), L"logs");
    EnsureDirectoryWide(logDir);
    StartProcessRequest req;
    req.name = "translate-llm";
    req.commandLine = command;
    req.workingDir = WideToUtf8(GetEffectiveDataDirectoryWide());
    req.mode = ExecMode::Pipe;
    req.openStdin = false;
    req.timeoutSec = kProcessTimeoutSec;
    req.logFile = WideToUtf8(PathJoinWide(logDir, Utf8ToWide(SanitizeFileNameComponent(batchId)) + L".log"));
    req.agentType = adapter.empty() ? selected : adapter;
    req.jsonStream = true;
    req.env["PC_TRANSLATE_INPUT_FILE"] = WideToUtf8(inputPath);
    req.supervisorGoal = "Translate Hancho text into " + lang;
    req.supervisorNotes = "Translation batch " + batchId + " with " + std::to_string(sources.size()) + " items";
    std::string startError;
    const int processId = pm_->StartProcess(req, startError);
    if (processId <= 0) {
        err = startError.empty() ? "failed to start the translation engine" : startError;
        RemoveFileWide(inputPath);
        return false;
    }

    const uint64_t deadline = GetNowTickMs() + static_cast<uint64_t>(kProcessTimeoutSec + 30) * 1000;
    for (;;) {
        ProcessSnapshot snap;
        if (!pm_->GetSnapshot(processId, snap) || snap.state != ProcState::Running) break;
        if (stop_.load() || GetNowTickMs() > deadline) {
            std::string killError;
            pm_->KillProcess(processId, killError);
            err = stop_.load() ? kStoppedMessage : "translation timed out";
            RemoveFileWide(inputPath);
            return false;
        }
        SleepMs(250);
    }
    const AgentExtractedResult result = ExtractAgentResult(*pm_, processId, true);
    RemoveFileWide(inputPath);
    if (!result.found || result.result.empty()) {
        err = "the translation engine gave no reply";
        return false;
    }
    return ParseTranslationReply(result.result, sources.size(), out, err);
}

void TranslationStore::WorkerLoop() {
    while (!stop_.load()) {
        ReloadConfig();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return stop_.load() || (CanRunLocked() && !queue_.empty());
            });
            SaveDirtyLocked(false);
        }
        if (stop_.load()) break;
        // One batch at a time: the translation worker is a single process at a time (concurrency 1).
        while (!stop_.load() && RunOneBatch()) {
        }
    }
}

TranslationStore* ActiveTranslationStore() {
    return g_activeStore.load();
}

void SetActiveTranslationStore(TranslationStore* store) {
    g_activeStore.store(store);
}

} // namespace pc
