// Headless smoke test for the translation cache and batch worker (src/translation_store.cpp).
// The LLM is replaced by a batch runner, so no engine process is started.

#include "translation_store.h"

#include "common.h"
#include "process_manager.h"
#include "task_lineage_view.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using pc::TranslationStore;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

struct Counter {
    int calls = 0;
    size_t items = 0;
    bool fail = false;
};

TranslationStore::BatchRunner MakeRunner(Counter& counter) {
    return [&counter](const std::string& lang, const std::string&, const std::string&,
                      const std::vector<std::string>& sources, std::vector<std::string>& out,
                      std::string& err) {
        ++counter.calls;
        counter.items += sources.size();
        if (counter.fail) {
            err = "fake failure";
            return false;
        }
        out.clear();
        for (const auto& source : sources) out.push_back(lang + ": " + source);
        return true;
    };
}

json Config(const std::string& mode = "auto", const std::string& cards = "all") {
    return json{{"translate_mode", mode}, {"translate_cards", cards},
                {"translate_engine", "dummy"}, {"translate_model", ""}};
}

void FreshDataDir(const std::string& name) {
    fs::path dir = fs::path(pc::GetTempDirectoryUtf8()) / ("hancho-translate-smoke-" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    pc::SetDataDirectoryOverride(dir.wstring());
}

void TestShouldTranslate() {
    CHECK(pc::ShouldTranslate("card.title", Config("auto", "all")));
    CHECK(pc::ShouldTranslate("card.body", Config("auto", "all")));
    CHECK(pc::ShouldTranslate("card.title", Config("auto", "title")));
    CHECK(!pc::ShouldTranslate("card.body", Config("auto", "title")));
    CHECK(!pc::ShouldTranslate("card.title", Config("auto", "off")));
    CHECK(!pc::ShouldTranslate("card.body", Config("auto", "off")));
    // translate_cards only covers task cards.
    CHECK(pc::ShouldTranslate("summary", Config("auto", "off")));
    CHECK(!pc::ShouldTranslate("summary", Config("off", "all")));
}

void TestParseReply() {
    std::vector<std::string> out;
    std::string err;
    CHECK(pc::ParseTranslationReply("[\"a\",\"b\"]", 2, out, err) && out.size() == 2 && out[1] == "b");
    CHECK(!pc::ParseTranslationReply("[\"a\"]", 2, out, err));
    CHECK(!pc::ParseTranslationReply("not json", 1, out, err));
    CHECK(pc::ParseTranslationReply("```json\n[\"x\"]\n```", 1, out, err) && out.size() == 1 && out[0] == "x");
    CHECK(!pc::ParseTranslationReply("[\"\"]", 1, out, err));
}

void TestRequestLookupAndDedupe() {
    FreshDataDir("basic");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());

    CHECK(!store.Lookup("Build failed", "ja").has_value());
    auto english = store.Lookup("Hello", "en");
    CHECK(english && *english == "Hello");
    CHECK(store.Request({"Build failed", "Build failed", ""}, "ja") == 1);
    CHECK(store.Request({"Build failed"}, "ja") == 0);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 1);
    auto translated = store.Lookup("Build failed", "ja");
    CHECK(translated && *translated == "ja: Build failed");
    CHECK(store.Request({"Build failed"}, "ja") == 0);
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 1);
    CHECK(store.Revision() > 0);
    // Same text, other language: a separate translation.
    CHECK(store.Request({"Build failed"}, "es") == 1);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 2);
    store.Stop();
}

void TestBatchLimits() {
    FreshDataDir("batches");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());
    std::vector<std::string> texts;
    for (int i = 0; i < 120; ++i) texts.push_back("text " + std::to_string(i));
    CHECK(store.Request(texts, "ja") == 120);
    int batches = 0;
    while (store.RunOneBatch()) ++batches;
    CHECK(batches == 3);
    CHECK(counter.calls == 3);
    CHECK(counter.items == 120);
    store.Stop();
}

void TestPersistsAcrossRestart() {
    FreshDataDir("persist");
    {
        TranslationStore store(nullptr, nullptr);
        Counter counter;
        store.SetBatchRunner(MakeRunner(counter));
        store.ApplyConfig(Config());
        CHECK(store.Request({"Saved text"}, "es") == 1);
        CHECK(store.RunOneBatch());
        store.Stop();
    }
    {
        TranslationStore store(nullptr, nullptr);
        Counter counter;
        store.SetBatchRunner(MakeRunner(counter));
        store.ApplyConfig(Config());
        auto saved = store.Lookup("Saved text", "es");
        CHECK(saved && *saved == "es: Saved text");
        CHECK(store.Request({"Saved text"}, "es") == 0);
        CHECK(!store.RunOneBatch());
        CHECK(counter.calls == 0);
        // The original text is checked: a different text never reads this translation.
        CHECK(!store.Lookup("Saved text!", "es").has_value());
        store.Stop();
    }
}

void TestConfigChangeKeepsCache() {
    FreshDataDir("config");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());
    CHECK(store.Request({"Keep me"}, "ja") == 1);
    CHECK(store.RunOneBatch());
    // Another engine: the cached translation is still used, nothing is asked again.
    store.ApplyConfig(json{{"translate_mode", "auto"}, {"translate_engine", "other"}});
    CHECK(store.Request({"Keep me"}, "ja") == 0);
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 1);
    // translate_mode off: nothing is queued at all.
    store.ApplyConfig(Config("off"));
    CHECK(store.Request({"Brand new"}, "ja") == 0);
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 1);
    store.Stop();
}

void TestCardSettingGatesRequests() {
    FreshDataDir("cards");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config("auto", "off"));
    CHECK(store.Request({"Card title"}, "ja", "card.title") == 0);
    CHECK(store.Request({"Card body"}, "ja", "card.body") == 0);
    CHECK(store.Request({"Summary"}, "ja", "summary") == 1);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 1);
    store.Stop();
}

void TestFailureBackoffAndClear() {
    FreshDataDir("failure");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    counter.fail = true;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());
    CHECK(store.Request({"Will fail"}, "ja") == 1);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 1);
    // One hour of backoff: asking again queues nothing and runs nothing.
    CHECK(store.Request({"Will fail"}, "ja") == 0);
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 1);
    json state = json::parse(store.Get({"Will fail"}, "ja", ""), nullptr, false);
    CHECK(state.is_object() && state["items"].size() == 1 && state["items"][0]["state"] == "failed");
    // translate_clear forgets the failure, so the text can be asked again.
    json cleared = json::parse(store.Clear("ja"), nullptr, false);
    CHECK(cleared.is_object() && cleared.value("ok", false));
    counter.fail = false;
    CHECK(store.Request({"Will fail"}, "ja") == 1);
    CHECK(store.RunOneBatch());
    auto translated = store.Lookup("Will fail", "ja");
    CHECK(translated && *translated == "ja: Will fail");
    store.Stop();
}

void TestEvictionLeastRecentlyUsed() {
    FreshDataDir("evict");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.SetMaxEntries(3);
    store.ApplyConfig(Config());
    CHECK(store.Request({"one", "two", "three"}, "ja") == 3);
    CHECK(store.RunOneBatch());
    CHECK(store.Lookup("one", "ja").has_value());  // "one" is now the most recently used
    CHECK(store.Request({"four"}, "ja") == 1);
    CHECK(store.RunOneBatch());
    CHECK(!store.Lookup("two", "ja").has_value());
    CHECK(store.Lookup("one", "ja").has_value());
    CHECK(store.Lookup("three", "ja").has_value());
    CHECK(store.Lookup("four", "ja").has_value());
    store.Stop();
}

void TestStatusIsJson() {
    FreshDataDir("status");
    TranslationStore store(nullptr, nullptr);
    store.ApplyConfig(Config());
    json status = json::parse(store.Status(), nullptr, false);
    CHECK(status.is_object() && status.value("ok", false));
    CHECK(status.value("engine", std::string()) == "dummy");
    store.Stop();
}

// Card 106-3: the dashboard's display table (pc::DisplayTextCache in task_lineage_view.h). Drawing never
// asks the store; these checks drive the table the way one list build does.
void TestDisplayTextCache() {
    using pc::DisplayTextCache;
    // English shows the original and never queues a miss.
    {
        DisplayTextCache cache;
        CHECK(cache.Display("Fix the build", "en", "", true) == "Fix the build");
        CHECK(cache.TakeMisses().empty());
    }
    // A miss shows the original and is queued once; after the answer, the translation is shown.
    DisplayTextCache cache;
    CHECK(cache.SetRevision(1) == false);
    CHECK(cache.Display("Fix the build", "ja", "", true) == "Fix the build");
    auto misses = cache.TakeMisses();
    CHECK(misses.size() == 1);
    CHECK(cache.TakeMisses().empty());
    for (const auto& m : misses) cache.Store(m.text, m.lang, m.field, "ビルドを直す", false);
    CHECK(cache.Display("Fix the build", "ja", "", true) == "ビルドを直す");
    CHECK(cache.CachedTranslation("Fix the build", "ja", "") == "ビルドを直す");
    CHECK(cache.CachedTranslation("Fix the build", "es", "").empty());
    CHECK(cache.TakeMisses().empty());

    // One fetch per list build: 100 redraws of the same rows make one fetch in total.
    {
        DisplayTextCache c;
        CHECK(c.SetRevision(7) == false);
        const std::vector<std::string> rows = {"Write the parser", "Add tests", "Update docs"};
        int fetches = 0;
        for (int draw = 0; draw < 100; ++draw) {
            for (const auto& text : rows) c.Display(text, "ja", "", true);
            auto queued = c.TakeMisses();
            if (queued.empty()) continue;
            ++fetches;
            for (const auto& m : queued) c.Store(m.text, m.lang, m.field, "訳: " + m.text, false);
        }
        CHECK(fetches == 1);
        CHECK(c.Display("Add tests", "ja", "", true) == "訳: Add tests");
    }

    // A text the core is still working on is not asked again until the revision changes.
    {
        DisplayTextCache c;
        CHECK(c.SetRevision(3) == false);
        c.Display("Pending row", "ja", "", true);
        for (const auto& m : c.TakeMisses()) c.Store(m.text, m.lang, m.field, "", true);
        CHECK(c.HasWaiting());
        CHECK(c.Display("Pending row", "ja", "", true) == "Pending row");
        CHECK(c.TakeMisses().empty());
        CHECK(c.SetRevision(4) == true);  // the translation landed: the table is dropped
        CHECK(!c.HasWaiting());
        CHECK(c.Display("Pending row", "ja", "", true) == "Pending row");
        CHECK(c.TakeMisses().size() == 1);
    }

    // Switching ja -> en -> ja keeps the ja translations: no new miss on the way back.
    {
        DisplayTextCache c;
        CHECK(c.SetRevision(1) == false);
        c.Display("Ship it", "ja", "", true);
        for (const auto& m : c.TakeMisses()) c.Store(m.text, m.lang, m.field, "出荷する", false);
        CHECK(c.Display("Ship it", "en", "", true) == "Ship it");
        CHECK(c.TakeMisses().empty());
        CHECK(c.Display("Ship it", "ja", "", true) == "出荷する");
        CHECK(c.TakeMisses().empty());
    }

    // translate_cards: "all" translates title and body, "title" only the title, "off" neither.
    // Task titles (field "") are not affected by the setting.
    {
        const std::vector<std::string> values = {"all", "title", "off"};
        const bool titleWant[] = {true, true, false};
        const bool bodyWant[] = {true, false, false};
        for (size_t i = 0; i < values.size(); ++i) {
            const json config = Config("auto", values[i]);
            DisplayTextCache c;
            c.SetRevision(1);
            c.Display("Card title", "ja", "card.title", pc::ShouldTranslate("card.title", config));
            c.Display("Card body", "ja", "card.body", pc::ShouldTranslate("card.body", config));
            c.Display("Task title", "ja", "", pc::ShouldTranslate("", config));
            bool sawTitle = false;
            bool sawBody = false;
            bool sawTask = false;
            for (const auto& m : c.TakeMisses()) {
                if (m.field == "card.title") sawTitle = true;
                if (m.field == "card.body") sawBody = true;
                if (m.field.empty()) sawTask = true;
            }
            CHECK(sawTitle == titleWant[i]);
            CHECK(sawBody == bodyWant[i]);
            CHECK(sawTask);
        }
    }
}

// Card 106-5: a foreign (non-English) field is kept as the original, translated to English, and
// the English moves into the field with the original in <field>Source.
void TestForeignFieldBecomesEnglish() {
    FreshDataDir("foreign");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());

    json job = {{"summary", "ビルドを直す"}};
    CHECK(!pc::NormalizeEnglishField(job, "summary", "", "", &store));
    CHECK(job["summary"] == "ビルドを直す");
    CHECK(job["summaryLang"] == "ja");
    CHECK(counter.calls == 0);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 1);
    CHECK(pc::NormalizeEnglishField(job, "summary", "", "", &store));
    CHECK(job["summary"] == "en: ビルドを直す");
    CHECK(job["summaryLang"] == "en");
    CHECK(job["summarySource"]["lang"] == "ja");
    CHECK(job["summarySource"]["text"] == "ビルドを直す");
    CHECK(pc::EnglishFieldForDisplay(job, "summary", "ja") == "ビルドを直す");
    CHECK(pc::EnglishFieldForDisplay(job, "summary", "en") == "en: ビルドを直す");

    // A sourceLang override makes Latin-script text foreign too (Spanish here).
    json spanish = {{"title", "Arreglar la compilación"}};
    CHECK(!pc::NormalizeEnglishField(spanish, "title", "es", "", &store));
    CHECK(spanish["titleLang"] == "es");
    CHECK(store.RunOneBatch());
    CHECK(pc::NormalizeEnglishField(spanish, "title", "es", "", &store));
    CHECK(spanish["title"] == "en: Arreglar la compilación");

    // English text is never queued or tagged.
    json english = {{"summary", "Fix the build"}};
    CHECK(pc::NormalizeEnglishField(english, "summary", "", "", &store));
    CHECK(!english.contains("summaryLang"));
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 2);
    store.Stop();
}

// Without an engine, or with translate_mode off, the original stays and only the language is tagged.
void TestForeignFieldWithoutEngineKeepsOriginal() {
    FreshDataDir("no-engine");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(json{{"translate_mode", "auto"}});
    json job = {{"title", "テスト"}};
    CHECK(!pc::NormalizeEnglishField(job, "title", "", "", &store));
    CHECK(job["title"] == "テスト");
    CHECK(job["titleLang"] == "ja");
    CHECK(!job.contains("titleSource"));
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 0);

    store.ApplyConfig(Config("off", "all"));
    json off = {{"summary", "テスト"}};
    CHECK(!pc::NormalizeEnglishField(off, "summary", "", "", &store));
    CHECK(off["summary"] == "テスト");
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 0);
    store.Stop();
}

// translate_cards gates the card fields: off translates neither, title translates only the title.
void TestTranslateCardsGatesFields() {
    FreshDataDir("cards-gate");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));

    store.ApplyConfig(Config("auto", "off"));
    json card = {{"title", "カード"}, {"body", "本文です"}};
    CHECK(!pc::NormalizeEnglishField(card, "title", "", "card.title", &store));
    CHECK(!pc::NormalizeEnglishField(card, "body", "", "card.body", &store));
    CHECK(card["title"] == "カード");
    CHECK(card["body"] == "本文です");
    CHECK(card["titleLang"] == "ja");
    CHECK(card["bodyLang"] == "ja");
    CHECK(!card.contains("bodySource"));
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 0);

    store.ApplyConfig(Config("auto", "title"));
    json titled = {{"title", "カード"}, {"body", "本文です"}};
    CHECK(!pc::NormalizeEnglishField(titled, "title", "", "card.title", &store));
    CHECK(!pc::NormalizeEnglishField(titled, "body", "", "card.body", &store));
    CHECK(store.RunOneBatch());
    CHECK(!store.RunOneBatch());
    CHECK(counter.calls == 1);
    CHECK(counter.items == 1);
    CHECK(pc::NormalizeEnglishField(titled, "title", "", "card.title", &store));
    CHECK(titled["title"] == "en: カード");
    CHECK(titled["body"] == "本文です");
    store.Stop();
}

void TestEnglishGetQueuesAndLands() {
    // card 106-5: translate_get with lang "en" queues a foreign text, leaves English alone, and
    // returns the translation once it has landed.
    FreshDataDir("english-get");
    TranslationStore store(nullptr, nullptr);
    Counter counter;
    store.SetBatchRunner(MakeRunner(counter));
    store.ApplyConfig(Config());

    const json first = json::parse(store.Get({"ビルドを直す", "Fix the build"}, "en", "card.title"));
    CHECK(first["items"][0]["state"] == "pending");
    CHECK(first["items"][1]["state"] == "original");
    CHECK(counter.calls == 0);
    CHECK(store.RunOneBatch());
    CHECK(counter.calls == 1);
    const json second = json::parse(store.Get({"ビルドを直す"}, "en", "card.title"));
    CHECK(second["items"][0]["state"] == "translated");
    CHECK(second["items"][0]["translated"] == "en: ビルドを直す");

    // translate_cards off excludes card titles: the original comes back and nothing is queued.
    store.ApplyConfig(Config("auto", "off"));
    const json excluded = json::parse(store.Get({"別のカード"}, "en", "card.title"));
    CHECK(excluded["items"][0]["state"] == "original");
    CHECK(!store.RunOneBatch());
    store.Stop();
}

} // namespace

int main() {
    TestShouldTranslate();
    TestParseReply();
    TestRequestLookupAndDedupe();
    TestBatchLimits();
    TestPersistsAcrossRestart();
    TestConfigChangeKeepsCache();
    TestCardSettingGatesRequests();
    TestFailureBackoffAndClear();
    TestEvictionLeastRecentlyUsed();
    TestStatusIsJson();
    TestDisplayTextCache();
    TestForeignFieldBecomesEnglish();
    TestForeignFieldWithoutEngineKeepsOriginal();
    TestTranslateCardsGatesFields();
    TestEnglishGetQueuesAndLands();
    if (g_failures == 0) {
        std::printf("translation_smoke: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "translation_smoke: %d check(s) failed\n", g_failures);
    return 1;
}
