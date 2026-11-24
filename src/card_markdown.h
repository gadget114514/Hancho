#pragma once

#include "nlohmann/json.hpp"
#include <functional>
#include <string>
#include <vector>

namespace pc {

// Markdown import and export for task cards (doc/task-card-store-cards.md, card 104-4).
//
// Import reads `## <number>: <title>` (or `## <number> <title>`) headings, one card per heading.
// The body runs to the next heading. Dependencies come from:
//   1. "依存:" lines (also "Depends on:", "Requires:", "Depende de:", "Requisitos:"), with arrow
//      chains (`a → b → c`) split into adjacent pairs,
//   2. the 前提 column of a Markdown table (also a "Depends on", "Requires" or "Requisitos" column),
//      with ranges (`103-1〜103-7`, `103-1~103-7`, `103-1–103-7`, `103-1..103-7`, `103-1 to 103-7`,
//      `103-1 a 103-7`) expanded,
//   3. "前提:" lines inside a card body.
// Card 106-5: a title or body that is not English is translated to English through translate_get
// (lang "en") when the core has the translation; otherwise the original is kept and a warning says so.
// The common promise (the paragraph after `---` that starts with 各タスクカードとも, or a
// "## 共通の約束" section) is appended to every card body.
//
// The functions here are pure except for the callback that talks to the task queue. Import and
// export only use the existing card_list / card_show / card_add / card_edit requests, so the
// same code runs for the local CLI and for a remote host.

struct CardMarkdownCard {
    std::string number;
    std::string title;
    std::string body;                 // dependency lines removed; common promise appended on import
    std::vector<std::string> after;   // prerequisite card numbers
    std::string state;                // export only: shown as "[state]" after the heading
};

struct CardMarkdownParseResult {
    std::vector<CardMarkdownCard> cards;  // document order; a repeated number keeps its first card
    std::vector<std::string> warnings;    // already in the language given to the parser
};

// Sends one request to the task queue and returns its JSON response. Returns false on a transport
// error (err is set). A response with "ok": false is returned as true with that JSON.
using CardMarkdownSend =
    std::function<bool(const nlohmann::json& req, nlohmann::json& resp, std::string& err)>;

struct CardMarkdownImportResult {
    bool ok = false;
    bool dryRun = false;
    int added = 0;
    int updated = 0;
    int skipped = 0;
    std::vector<std::string> actions;   // one localized line per card
    std::vector<std::string> warnings;  // localized
    std::vector<std::string> errors;    // localized; non-empty means ok is false
};

// Parses text and applies it to task taskId through send. With dryRun, only the plan is returned
// and nothing is written. lang is "ja", "en" or "es" (anything else is English).
CardMarkdownParseResult ParseCardMarkdown(const std::string& text, const std::string& lang);

// translateCards: "all", "title" or "off" (card 106-5, overrides the config for this run). Empty means
// the config of the core decides. Translation is skipped on a dry run.
CardMarkdownImportResult ImportCardMarkdown(const std::string& taskId, const std::string& text,
                                            bool dryRun, const std::string& lang,
                                            const CardMarkdownSend& send,
                                            const std::string& translateCards = "");

// Renders cards in the same shape that ParseCardMarkdown reads.
std::string RenderCardMarkdown(const std::vector<CardMarkdownCard>& cards);

// Reads every card of taskId (with bodies and dependencies) and renders it as Markdown. lang "en"
// (or empty) writes the stored English; another language writes the translation (card 106-5).
bool ExportCardMarkdown(const std::string& taskId, const std::string& lang,
                        const CardMarkdownSend& send, std::string& outMarkdown,
                        std::string& outError);

// Card 106-5, `card translate --task`: translates the stored titles and bodies that are not English
// (translate_get, then card_edit for each one that is ready). Items not ready yet stay as they are
// and are counted as pending; run it again later.
struct CardTranslateResult {
    bool ok = false;
    int updated = 0;
    int pending = 0;
    std::vector<std::string> actions;   // one localized line per card that changed
    std::vector<std::string> warnings;  // localized
    std::vector<std::string> errors;    // localized; non-empty means ok is false
};

CardTranslateResult TranslateCardsInTask(const std::string& taskId, const std::string& translateCards,
                                         const std::string& lang, const CardMarkdownSend& send);

// JSON form of an import result, for --json output.
nlohmann::json CardMarkdownImportResultJson(const CardMarkdownImportResult& result);

// Picks the text for a language, using the same rule as CliTr: ja, es, otherwise English.
inline std::string CardMdTr(const std::string& lang, const std::string& ja, const std::string& en,
                            const std::string& es) {
    if (lang == "ja") return ja;
    if (lang == "es") return es;
    return en;
}

} // namespace pc
