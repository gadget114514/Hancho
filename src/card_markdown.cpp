#include "card_markdown.h"

#include "common.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace pc {

namespace {

const char kArrow[] = "\xE2\x86\x92";        // →
const char kFullColon[] = "\xEF\xBC\x9A";    // ：
const char kIdeoComma[] = "\xE3\x80\x81";    // 、
const char kFullSemicolon[] = "\xEF\xBC\x9B"; // ；
const char kRangeWave[] = "\xE3\x80\x9C";    // 〜
const char kRangeFullWave[] = "\xEF\xBD\x9E"; // ～
const char kRangeDash[] = "\xE2\x80\x93";     // en dash
const char kPromiseStart[] = "各タスクカードとも";
const char kPromiseHeading[] = "共通の約束";
const char kBom[] = "\xEF\xBB\xBF";

using Edge = std::pair<std::string, std::string>;  // prerequisite -> dependent

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool StartsAt(const std::string& s, size_t pos, const char* prefix) {
    if (pos > s.size()) return false;
    return s.compare(pos, std::char_traits<char>::length(prefix), prefix) == 0;
}

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitBy(const std::string& s, const std::string& delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t p = s.find(delim, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + delim.size();
    }
    return out;
}

std::string ReplaceAll(const std::string& s, const std::string& from, const std::string& to) {
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t p = s.find(from, start);
        if (p == std::string::npos) {
            out += s.substr(start);
            break;
        }
        out += s.substr(start, p - start);
        out += to;
        start = p + from.size();
    }
    return out;
}

std::string JoinStrings(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += sep;
        out += items[i];
    }
    return out;
}

void AddUnique(std::vector<std::string>& items, const std::string& value) {
    if (std::find(items.begin(), items.end(), value) == items.end()) items.push_back(value);
}

bool ParseSmallInt(const std::string& s, long long& out) {
    if (s.empty() || s.size() > 6) return false;
    long long v = 0;
    for (char c : s) {
        if (!IsDigit(c)) return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// "<1 to 3 digits>-<digits>" at pos, e.g. 102-3. The token must not run on into more digits.
bool ReadRef(const std::string& s, size_t pos, std::string& outRef, size_t& outEnd) {
    size_t i = pos;
    while (i < s.size() && IsDigit(s[i])) ++i;
    const size_t firstLen = i - pos;
    if (firstLen == 0 || firstLen > 3 || i >= s.size() || s[i] != '-') return false;
    size_t j = i + 1;
    while (j < s.size() && IsDigit(s[j])) ++j;
    if (j == i + 1) return false;
    outRef = s.substr(pos, j - pos);
    outEnd = j;
    return true;
}

bool IsCardNumberToken(const std::string& token) {
    std::string ref;
    size_t end = 0;
    return ReadRef(token, 0, ref, end) && end == token.size();
}

bool IsKnownState(const std::string& s) {
    return s == "planned" || s == "running" || s == "succeeded" || s == "failed" ||
           s == "closed" || s == "cancelled";
}

// A dependency text that means "no dependency".
bool IsNoneText(const std::string& text) {
    std::string t = Trim(text);
    if (!t.empty() && t.size() >= 3 && t.compare(t.size() - 3, 3, "。") == 0) t.resize(t.size() - 3);
    t = Trim(t);
    return t.empty() || t == "なし" || t == "無し" || t == "-" || t == "—" || t == "none" ||
           t == "None" || t == "ninguna";
}

size_t RangeSepLengthAt(const std::string& s, size_t pos) {
    if (StartsAt(s, pos, kRangeWave)) return std::char_traits<char>::length(kRangeWave);
    if (StartsAt(s, pos, kRangeFullWave)) return std::char_traits<char>::length(kRangeFullWave);
    if (StartsAt(s, pos, kRangeDash)) return std::char_traits<char>::length(kRangeDash);
    if (StartsAt(s, pos, "..")) return 2;
    if (StartsAt(s, pos, "~")) return 1;
    // English words count only when a space comes before them: "103-1 to 103-3", "103-1 a 103-3".
    if (pos > 0 && s[pos - 1] == ' ' && StartsAt(s, pos, "to ")) return 2;
    if (pos > 0 && s[pos - 1] == ' ' && StartsAt(s, pos, "a ")) return 1;
    return 0;
}

// Expands a range such as 103-1〜103-7 (or 103-1〜7) into 103-1 .. 103-7. When the two ends do
// not fit, both ends are kept and a warning is added.
void ExpandRange(const std::string& a, const std::string& bRaw, std::vector<std::string>& out,
                 std::vector<std::string>& warnings, const std::string& lang) {
    const size_t dashA = a.rfind('-');
    const std::string prefix = a.substr(0, dashA);
    const std::string startStr = a.substr(dashA + 1);
    const size_t dashB = bRaw.rfind('-');
    const std::string endPrefix = (dashB == std::string::npos) ? prefix : bRaw.substr(0, dashB);
    const std::string endStr = (dashB == std::string::npos) ? bRaw : bRaw.substr(dashB + 1);
    long long s0 = 0;
    long long e0 = 0;
    if (endPrefix != prefix || !ParseSmallInt(startStr, s0) || !ParseSmallInt(endStr, e0) ||
        e0 < s0 || e0 - s0 > 500) {
        out.push_back(a);
        out.push_back(dashB == std::string::npos ? prefix + "-" + endStr : bRaw);
        warnings.push_back(CardMdTr(lang,
            "範囲を展開できない: " + a + "〜" + bRaw, "range not expanded: " + a + " to " + bRaw,
            "rango no expandido: " + a + " a " + bRaw));
        return;
    }
    for (long long k = s0; k <= e0; ++k) {
        out.push_back(prefix + "-" + std::to_string(k));
    }
}

// Every card number in s, in order. Ranges are expanded. Digit runs that are not card numbers
// (dates such as 2026-11-23) are skipped.
std::vector<std::string> ExtractRefs(const std::string& s, std::vector<std::string>& warnings,
                                     const std::string& lang) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const bool boundary = (i == 0) || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) ||
                                             s[i - 1] == '-' || s[i - 1] == '_' || s[i - 1] == '.');
        if (!(IsDigit(s[i]) && boundary)) {
            ++i;
            continue;
        }
        std::string first;
        size_t firstEnd = 0;
        if (!ReadRef(s, i, first, firstEnd)) {
            while (i < s.size() && IsDigit(s[i])) ++i;
            continue;
        }
        size_t j = firstEnd;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        const size_t sepLen = RangeSepLengthAt(s, j);
        if (sepLen == 0) {
            out.push_back(first);
            i = firstEnd;
            continue;
        }
        size_t k = j + sepLen;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
        std::string second;
        size_t secondEnd = 0;
        if (ReadRef(s, k, second, secondEnd)) {
            ExpandRange(first, second, out, warnings, lang);
            i = secondEnd;
            continue;
        }
        size_t d = k;
        while (d < s.size() && IsDigit(s[d])) ++d;
        if (d > k && d - k <= 3 && (d == s.size() || s[d] != '-')) {
            ExpandRange(first, s.substr(k, d - k), out, warnings, lang);
            i = d;
            continue;
        }
        warnings.push_back(CardMdTr(lang, "範囲の終わりを読めない: " + s, "range end not readable: " + s,
                                    "fin del rango no legible: " + s));
        out.push_back(first);
        i = firstEnd;
    }
    return out;
}

bool HasArrow(const std::string& s) {
    return s.find(kArrow) != std::string::npos || s.find("->") != std::string::npos;
}

// Splits "a → b → c" into adjacent pairs. Each arrow segment may hold several numbers
// ("102-2, 102-4 → 102-6"); every number in one segment is a prerequisite of every number in
// the next segment.
void AddChainEdges(const std::string& text, std::vector<Edge>& edges,
                   std::vector<std::string>& warnings, const std::string& lang) {
    std::string t = ReplaceAll(text, "->", kArrow);
    t = ReplaceAll(t, kIdeoComma, "\x01");
    t = ReplaceAll(t, kFullSemicolon, "\x01");
    t = ReplaceAll(t, ";", "\x01");
    for (const std::string& chain : SplitBy(t, "\x01")) {
        if (!HasArrow(chain)) continue;
        const std::vector<std::string> parts = SplitBy(chain, kArrow);
        std::vector<std::vector<std::string>> nums;
        for (const std::string& part : parts) {
            std::vector<std::string> refs = ExtractRefs(part, warnings, lang);
            if (refs.empty()) {
                warnings.push_back(CardMdTr(lang, "読めない依存: " + Trim(part),
                                            "unreadable dependency: " + Trim(part),
                                            "dependencia no legible: " + Trim(part)));
            }
            nums.push_back(std::move(refs));
        }
        for (size_t i = 0; i + 1 < nums.size(); ++i) {
            for (const auto& a : nums[i]) {
                for (const auto& b : nums[i + 1]) edges.emplace_back(a, b);
            }
        }
    }
}

// "依存:" or "前提:" (ASCII or full-width colon) at the start of a bare line. rest is the text
// after the colon.
bool MatchLabel(const std::string& bare, const std::string& label, std::string& rest) {
    if (bare.compare(0, label.size(), label) != 0) return false;
    size_t p = label.size();
    if (p < bare.size() && bare[p] == ':') {
        rest = Trim(bare.substr(p + 1));
        return true;
    }
    if (StartsAt(bare, p, kFullColon)) {
        rest = Trim(bare.substr(p + 3));
        return true;
    }
    return false;
}

// The dependency label in any supported language (card 106-5): 依存, Depends on, Requires,
// Depende de, Requisitos.
bool MatchDependsLabel(const std::string& bare, std::string& rest) {
    return MatchLabel(bare, "依存", rest) || MatchLabel(bare, "Depends on", rest) ||
           MatchLabel(bare, "Requires", rest) || MatchLabel(bare, "Depende de", rest) ||
           MatchLabel(bare, "Requisitos", rest);
}

// A table header cell that names the prerequisite column in any supported language.
bool IsPrereqHeader(const std::string& cell) {
    return cell.find("前提") != std::string::npos || cell.find("Depends on") != std::string::npos ||
           cell.find("Requires") != std::string::npos || cell.find("Requisitos") != std::string::npos;
}

// A bullet "- x" or "* x" without the marker; other lines are returned trimmed.
std::string StripBullet(const std::string& trimmed) {
    if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*') && trimmed[1] == ' ') {
        return Trim(trimmed.substr(2));
    }
    return trimmed;
}

// The text of a level 1 or 2 heading. Level 3 and deeper stay in the body.
bool HeadingText(const std::string& line, std::string& text) {
    size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    if (n < 1 || n > 2 || n >= line.size() || line[n] != ' ') return false;
    text = Trim(line.substr(n));
    return true;
}

// "102-3: title [planned]" or "102-3 title". Returns false when the first token is not a card
// number, so the heading is a plain section.
bool ParseCardHeading(const std::string& text, std::string& number, std::string& title,
                      std::string& state) {
    size_t p = 0;
    while (p < text.size() && text[p] != ' ' && text[p] != '\t' && text[p] != ':' &&
           !StartsAt(text, p, kFullColon)) {
        ++p;
    }
    const std::string token = text.substr(0, p);
    if (!IsCardNumberToken(token)) return false;
    number = token;
    std::string rest = Trim(text.substr(p));
    if (!rest.empty() && rest[0] == ':') {
        rest = Trim(rest.substr(1));
    } else if (StartsAt(rest, 0, kFullColon)) {
        rest = Trim(rest.substr(3));
    }
    state.clear();
    if (!rest.empty() && rest.back() == ']') {
        const size_t open = rest.rfind('[');
        if (open != std::string::npos) {
            const std::string inner = rest.substr(open + 1, rest.size() - open - 2);
            if (IsKnownState(inner)) {
                state = inner;
                rest = Trim(rest.substr(0, open));
            }
        }
    }
    title = rest;
    return true;
}

std::vector<std::string> TableCells(const std::string& trimmed) {
    std::string inner = trimmed;
    if (!inner.empty() && inner.front() == '|') inner.erase(0, 1);
    if (!inner.empty() && inner.back() == '|') inner.pop_back();
    std::vector<std::string> cells = SplitBy(inner, "|");
    for (auto& c : cells) c = Trim(c);
    return cells;
}

bool IsSeparatorRow(const std::vector<std::string>& cells) {
    if (cells.empty()) return false;
    for (const auto& c : cells) {
        if (c.empty()) continue;
        for (char ch : c) {
            if (ch != '-' && ch != ':') return false;
        }
    }
    return true;
}

struct CardDraft {
    CardMarkdownCard card;
    std::vector<std::string> bodyLines;
    std::vector<std::string> ownDeps;  // from "前提:" and "依存:" lines inside the card
    bool duplicate = false;
};

std::string BodyFromLines(const std::vector<std::string>& lines) {
    size_t b = 0;
    size_t e = lines.size();
    while (b < e && Trim(lines[b]).empty()) ++b;
    while (e > b && Trim(lines[e - 1]).empty()) --e;
    std::vector<std::string> kept(lines.begin() + static_cast<std::ptrdiff_t>(b),
                                  lines.begin() + static_cast<std::ptrdiff_t>(e));
    return JoinStrings(kept, "\n");
}

} // namespace

CardMarkdownParseResult ParseCardMarkdown(const std::string& textIn, const std::string& lang) {
    CardMarkdownParseResult result;
    std::string text = textIn;
    if (text.compare(0, 3, kBom) == 0) text.erase(0, 3);
    std::vector<std::string> lines = SplitBy(text, "\n");
    for (auto& line : lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }

    std::vector<CardDraft> drafts;
    std::map<std::string, size_t> firstIndex;
    std::vector<Edge> edges;
    std::vector<std::pair<std::string, std::vector<std::string>>> tableDeps;
    std::vector<std::string> promise;
    bool promiseMode = false;
    int cur = -1;       // index into drafts while inside a card body
    int prereqCol = -1; // the 前提 column of the table being read

    auto& warnings = result.warnings;

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        const std::string trimmed = Trim(line);

        std::string headingText;
        if (HeadingText(line, headingText)) {
            cur = -1;
            prereqCol = -1;
            promiseMode = false;
            std::string number;
            std::string title;
            std::string state;
            if (ParseCardHeading(headingText, number, title, state)) {
                CardDraft d;
                d.card.number = number;
                d.card.title = title;
                d.card.state = state;
                if (firstIndex.count(number) > 0) {
                    d.duplicate = true;
                    warnings.push_back(CardMdTr(lang,
                        "同じ番号の見出しが重なっている（後のものは無視）: " + number,
                        "card heading repeated; the later one is ignored: " + number,
                        "encabezado repetido; se ignora el posterior: " + number));
                } else {
                    firstIndex[number] = drafts.size();
                }
                drafts.push_back(std::move(d));
                cur = static_cast<int>(drafts.size()) - 1;
                if (title.empty()) {
                    drafts.back().card.title = number;
                    warnings.push_back(CardMdTr(lang,
                        "見出しに title がない（番号を title に使う）: " + number,
                        "heading has no title; the number is used: " + number,
                        "el encabezado no tiene título; se usa el número: " + number));
                }
            } else {
                promiseMode = headingText.find(kPromiseHeading) != std::string::npos;
            }
            continue;
        }

        if (cur >= 0) {
            CardDraft& d = drafts[static_cast<size_t>(cur)];
            if (trimmed == "---") {
                // A rule ends the card when the next text is a heading or the common promise.
                size_t nx = li + 1;
                while (nx < lines.size() && Trim(lines[nx]).empty()) ++nx;
                const bool ends = nx >= lines.size() || HeadingText(lines[nx], headingText) ||
                                  Trim(lines[nx]).rfind(kPromiseStart, 0) == 0;
                if (ends) {
                    cur = -1;
                    prereqCol = -1;
                } else {
                    d.bodyLines.push_back(line);
                }
                continue;
            }
            const std::string bare = StripBullet(trimmed);
            std::string rest;
            if (MatchDependsLabel(bare, rest)) {
                if (HasArrow(rest)) {
                    AddChainEdges(rest, edges, warnings, lang);
                } else if (!IsNoneText(rest)) {
                    std::vector<std::string> refs = ExtractRefs(rest, warnings, lang);
                    if (refs.empty()) {
                        warnings.push_back(CardMdTr(lang, "読めない依存: " + bare,
                                                    "unreadable dependency: " + bare,
                                                    "dependencia no legible: " + bare));
                    }
                    for (const auto& r : refs) AddUnique(d.ownDeps, r);
                }
                continue;
            }
            if (MatchLabel(bare, "前提", rest)) {
                if (!IsNoneText(rest)) {
                    std::vector<std::string> refs = ExtractRefs(rest, warnings, lang);
                    if (refs.empty()) {
                        warnings.push_back(CardMdTr(lang, "読めない前提: " + bare,
                                                    "unreadable prerequisite: " + bare,
                                                    "prerrequisito no legible: " + bare));
                    }
                    for (const auto& r : refs) AddUnique(d.ownDeps, r);
                }
                continue;
            }
            d.bodyLines.push_back(line);
            continue;
        }

        // Outside a card: the common promise, document-level 依存 lines, and the 前提 column.
        if (trimmed.empty() || trimmed[0] != '|') prereqCol = -1;
        if (promiseMode || trimmed.rfind(kPromiseStart, 0) == 0) {
            promiseMode = true;
            promise.push_back(line);
            continue;
        }
        const std::string bare = StripBullet(trimmed);
        std::string rest;
        if (MatchDependsLabel(bare, rest)) {
            if (HasArrow(rest)) {
                AddChainEdges(rest, edges, warnings, lang);
            } else if (!IsNoneText(rest)) {
                warnings.push_back(CardMdTr(lang,
                    "矢印のない依存の行は読まない: " + bare,
                    "dependency line without an arrow is ignored: " + bare,
                    "línea de dependencia sin flecha, se ignora: " + bare));
            }
            continue;
        }
        if (!trimmed.empty() && trimmed[0] == '|') {
            const std::vector<std::string> cells = TableCells(trimmed);
            if (IsSeparatorRow(cells)) continue;
            int found = -1;
            for (size_t c = 0; c < cells.size(); ++c) {
                if (IsPrereqHeader(cells[c])) {
                    found = static_cast<int>(c);
                    break;
                }
            }
            if (found >= 0) {
                prereqCol = found;  // header row of a table with a 前提 column
                continue;
            }
            if (prereqCol >= 0 && static_cast<size_t>(prereqCol) < cells.size() && !cells.empty()) {
                std::vector<std::string> firstRefs = ExtractRefs(cells[0], warnings, lang);
                if (!firstRefs.empty()) {
                    tableDeps.emplace_back(firstRefs.front(),
                                           ExtractRefs(cells[static_cast<size_t>(prereqCol)], warnings, lang));
                }
            }
            continue;
        }
    }

    const std::string promiseText = Trim(JoinStrings(promise, "\n"));

    std::set<std::string> docNumbers;
    for (const auto& d : drafts) {
        if (!d.duplicate) docNumbers.insert(d.card.number);
    }

    for (const auto& [dep, dependent] : edges) {
        if (docNumbers.count(dependent) == 0) {
            warnings.push_back(CardMdTr(lang,
                "文書にないカードへの依存は無視: " + dep + " → " + dependent,
                "dependency on a card outside this document is ignored: " + dep + " -> " + dependent,
                "dependencia de una tarjeta fuera de este documento, se ignora: " + dep + " -> " + dependent));
        }
    }
    for (const auto& [number, deps] : tableDeps) {
        (void)deps;
        if (docNumbers.count(number) == 0) {
            warnings.push_back(CardMdTr(lang,
                "前提の表の行に対応するカードがない: " + number,
                "table row has no card in this document: " + number,
                "fila de la tabla sin tarjeta en este documento: " + number));
        }
    }

    for (auto& d : drafts) {
        if (d.duplicate) continue;
        CardMarkdownCard card = d.card;
        std::vector<std::string> deps = d.ownDeps;
        for (const auto& [number, tdeps] : tableDeps) {
            if (number != card.number) continue;
            for (const auto& t : tdeps) AddUnique(deps, t);
        }
        for (const auto& [dep, dependent] : edges) {
            if (dependent == card.number) AddUnique(deps, dep);
        }
        std::vector<std::string> cleaned;
        for (const auto& dep : deps) {
            if (dep == card.number) {
                warnings.push_back(CardMdTr(lang,
                    "カードが自分自身に依存: " + card.number + "（無視）",
                    "card depends on itself; ignored: " + card.number,
                    "la tarjeta depende de sí misma; se ignora: " + card.number));
                continue;
            }
            AddUnique(cleaned, dep);
        }
        card.after = cleaned;
        std::string body = BodyFromLines(d.bodyLines);
        if (!promiseText.empty()) {
            body = body.empty() ? promiseText : body + "\n\n" + promiseText;
        }
        card.body = body;
        result.cards.push_back(std::move(card));
    }
    return result;
}

std::string RenderCardMarkdown(const std::vector<CardMarkdownCard>& cards) {
    std::string out;
    for (const auto& c : cards) {
        out += "## " + c.number + ": " + c.title;
        if (!c.state.empty()) out += " [" + c.state + "]";
        out += "\n\n";
        const std::string body = Trim(c.body);
        if (!body.empty()) out += body + "\n\n";
        if (!c.after.empty()) out += "前提: " + JoinStrings(c.after, ", ") + "\n\n";
    }
    return out;
}

namespace {

// The card setting that excludes a field from translation (card 106-5). Empty means the core's
// config decides, so nothing is excluded here.
bool CardFieldExcluded(const std::string& field, const std::string& translateCards) {
    if (translateCards.empty()) return false;
    if (translateCards == "off") return true;
    return translateCards == "title" && field == "card.body";
}

// Asks the core for one text's translation (translate_get). out is the translation when state is
// "translated", otherwise the text itself. Returns false when the request failed (err is set).
bool TranslateGetOne(const std::string& text, const std::string& lang, const std::string& field,
                     const CardMarkdownSend& send, std::string& out, std::string& state, std::string& err) {
    out = text;
    state = "unavailable";
    nlohmann::json req = {{"action", "translate_get"}, {"lang", lang}, {"field", field},
                          {"texts", nlohmann::json::array({text})}};
    nlohmann::json resp;
    if (!send(req, resp, err)) return false;
    if (!resp.is_object() || !resp.value("ok", false)) {
        err = resp.is_object() ? resp.value("error", std::string("translate failed"))
                               : std::string("translate failed");
        return false;
    }
    const nlohmann::json items = resp.value("items", nlohmann::json::array());
    if (!items.is_array() || items.empty() || !items[0].is_object()) return true;
    state = items[0].value("state", std::string("unavailable"));
    if (state == "translated" && items[0].contains("translated") && items[0]["translated"].is_string()) {
        out = items[0]["translated"].get<std::string>();
    }
    return true;
}

// Card 106-5, import: the English text of one title or body. The original is returned when it is
// English, excluded by translateCards, or not translated yet (a warning says so for the last two).
std::string EnglishCardField(const std::string& text, const std::string& field, const std::string& translateCards,
                             const std::string& lang, const CardMarkdownSend& send,
                             std::vector<std::string>& warnings) {
    if (text.empty() || !IsNonEnglishText(text) || CardFieldExcluded(field, translateCards)) return text;
    std::string out;
    std::string state;
    std::string err;
    if (!TranslateGetOne(text, "en", field, send, out, state, err)) {
        warnings.push_back(CardMdTr(lang,
            "訳を頼めなかった（原文のまま保存）: " + err,
            "could not request a translation; the original was stored: " + err,
            "no se pudo pedir una traducción; se guardó el original: " + err));
        return text;
    }
    if (state == "translated") return out;
    if (state != "original") {
        warnings.push_back(CardMdTr(lang,
            "英語の訳がまだない（原文のまま保存）: " + field + "。card translate --task で後から訳せる",
            "no English translation yet; the original was stored: " + field +
                ". Run card translate --task later",
            "aún no hay traducción al inglés; se guardó el original: " + field +
                ". Use card translate --task más tarde"));
    }
    return text;
}

} // namespace

CardMarkdownImportResult ImportCardMarkdown(const std::string& taskId, const std::string& text,
                                            bool dryRun, const std::string& lang,
                                            const CardMarkdownSend& send,
                                            const std::string& translateCards) {
    CardMarkdownImportResult r;
    r.dryRun = dryRun;
    if (taskId.empty()) {
        r.errors.push_back(CardMdTr(lang, "--task が必要", "--task is required", "se requiere --task"));
        return r;
    }
    CardMarkdownParseResult parsed = ParseCardMarkdown(text, lang);
    r.warnings = parsed.warnings;
    if (!dryRun) {
        for (auto& card : parsed.cards) {
            card.title = EnglishCardField(card.title, "card.title", translateCards, lang, send, r.warnings);
            card.body = EnglishCardField(card.body, "card.body", translateCards, lang, send, r.warnings);
        }
    }

    nlohmann::json listReq = {{"action", "card_list"}, {"taskId", taskId}};
    nlohmann::json listResp;
    std::string err;
    if (!send(listReq, listResp, err)) {
        r.errors.push_back(err);
        return r;
    }
    if (!listResp.is_object() || !listResp.value("ok", false)) {
        r.errors.push_back(listResp.is_object() ? listResp.value("error", std::string("card list failed"))
                                                : std::string("card list failed"));
        return r;
    }
    std::map<std::string, nlohmann::json> existing;
    for (const auto& c : listResp.value("cards", nlohmann::json::array())) {
        if (c.is_object()) existing[c.value("number", std::string())] = c;
    }
    std::set<std::string> docNumbers;
    for (const auto& c : parsed.cards) docNumbers.insert(c.number);

    struct Plan {
        CardMarkdownCard card;
        bool update = false;
        std::vector<std::string> after;
    };
    std::vector<Plan> plans;
    for (const auto& card : parsed.cards) {
        Plan p;
        p.card = card;
        auto it = existing.find(card.number);
        if (it != existing.end()) {
            const std::string state = it->second.value("state", std::string());
            if (state != "planned") {
                ++r.skipped;
                r.actions.push_back(CardMdTr(lang, "飛ばす ", "skip ", "omitir ") + card.number +
                                    " [" + state + "]");
                r.warnings.push_back(CardMdTr(lang,
                    "カード " + card.number + " は " + state + " なので上書きしない（飛ばした）",
                    "card " + card.number + " is " + state + "; not overwritten (skipped)",
                    "la tarjeta " + card.number + " está " + state + "; no se sobrescribe (omitida)"));
                continue;
            }
            p.update = true;
        }
        for (const auto& dep : card.after) {
            if (existing.count(dep) > 0 || docNumbers.count(dep) > 0) {
                p.after.push_back(dep);
            } else {
                r.warnings.push_back(CardMdTr(lang,
                    "カード " + card.number + " の依存 " + dep + " はこの task にないので無視",
                    "card " + card.number + " depends on " + dep + ", which is not in this task; dropped",
                    "la tarjeta " + card.number + " depende de " + dep + ", que no está en esta tarea; se descarta"));
            }
        }
        if (p.update) {
            ++r.updated;
        } else {
            ++r.added;
        }
        std::string line = CardMdTr(lang, p.update ? "上書き " : "追加 ", p.update ? "update " : "add ",
                                    p.update ? "actualizar " : "añadir ") +
                           card.number + ": " + card.title;
        if (!p.after.empty()) line += " (after " + JoinStrings(p.after, ", ") + ")";
        r.actions.push_back(line);
        plans.push_back(std::move(p));
    }

    if (dryRun) {
        r.ok = r.errors.empty();
        return r;
    }

    // Pass 1 creates or rewrites every card without dependencies, so that every dependency
    // exists when pass 2 sets them. Pass 2 is needed for cards that have dependencies, and for
    // updated cards so that their old dependencies are cleared when the document has none.
    std::vector<const Plan*> needDeps;
    for (const auto& p : plans) {
        nlohmann::json req = {{"action", p.update ? "card_edit" : "card_add"},
                              {"taskId", taskId},
                              {"number", p.card.number},
                              {"title", p.card.title},
                              {"body", p.card.body}};
        nlohmann::json resp;
        if (!send(req, resp, err)) {
            r.errors.push_back(p.card.number + ": " + err);
            continue;
        }
        if (!resp.is_object() || !resp.value("ok", false)) {
            r.errors.push_back(p.card.number + ": " +
                               (resp.is_object() ? resp.value("error", std::string("failed")) : std::string("failed")));
            continue;
        }
        if (p.update || !p.after.empty()) needDeps.push_back(&p);
    }
    for (const Plan* p : needDeps) {
        nlohmann::json req = {{"action", "card_edit"},
                              {"taskId", taskId},
                              {"number", p->card.number},
                              {"after", nlohmann::json(p->after)}};
        nlohmann::json resp;
        if (!send(req, resp, err)) {
            r.errors.push_back(p->card.number + ": " + err);
            continue;
        }
        if (!resp.is_object() || !resp.value("ok", false)) {
            r.errors.push_back(p->card.number + " (after): " +
                               (resp.is_object() ? resp.value("error", std::string("failed")) : std::string("failed")));
        }
    }
    r.ok = r.errors.empty();
    return r;
}

bool ExportCardMarkdown(const std::string& taskId, const std::string& lang,
                        const CardMarkdownSend& send, std::string& outMarkdown,
                        std::string& outError) {
    outMarkdown.clear();
    outError.clear();
    nlohmann::json listReq = {{"action", "card_list"}, {"taskId", taskId}};
    nlohmann::json listResp;
    if (!send(listReq, listResp, outError)) return false;
    if (!listResp.is_object() || !listResp.value("ok", false)) {
        outError = listResp.is_object() ? listResp.value("error", std::string("card list failed"))
                                        : std::string("card list failed");
        return false;
    }
    std::vector<CardMarkdownCard> cards;
    for (const auto& item : listResp.value("cards", nlohmann::json::array())) {
        if (!item.is_object()) continue;
        CardMarkdownCard c;
        c.number = item.value("number", std::string());
        c.title = item.value("title", std::string());
        c.state = item.value("state", std::string());
        if (item.contains("after") && item["after"].is_array()) {
            for (const auto& dep : item["after"]) {
                if (dep.is_string()) c.after.push_back(dep.get<std::string>());
            }
        }
        nlohmann::json showReq = {{"action", "card_show"},
                                  {"id", item.value("directionId", std::string())},
                                  {"taskId", taskId}};
        nlohmann::json showResp;
        if (!send(showReq, showResp, outError)) return false;
        if (!showResp.is_object() || !showResp.value("ok", false)) {
            outError = showResp.is_object() ? showResp.value("error", std::string("card show failed"))
                                            : std::string("card show failed");
            return false;
        }
        if (showResp.contains("card") && showResp["card"].is_object()) {
            c.body = showResp["card"].value("body", std::string());
        }
        cards.push_back(std::move(c));
    }
    if (!lang.empty() && lang != "en") {
        for (auto& c : cards) {
            std::string out;
            std::string state;
            std::string err;
            if (TranslateGetOne(c.title, lang, "card.title", send, out, state, err)) c.title = out;
            if (!c.body.empty() && TranslateGetOne(c.body, lang, "card.body", send, out, state, err)) {
                c.body = out;
            }
        }
    }
    outMarkdown = RenderCardMarkdown(cards);
    return true;
}

CardTranslateResult TranslateCardsInTask(const std::string& taskId, const std::string& translateCards,
                                         const std::string& lang, const CardMarkdownSend& send) {
    CardTranslateResult r;
    if (taskId.empty()) {
        r.errors.push_back(CardMdTr(lang, "--task が必要", "--task is required", "se requiere --task"));
        return r;
    }
    nlohmann::json listReq = {{"action", "card_list"}, {"taskId", taskId}};
    nlohmann::json listResp;
    std::string err;
    if (!send(listReq, listResp, err)) {
        r.errors.push_back(err);
        return r;
    }
    if (!listResp.is_object() || !listResp.value("ok", false)) {
        r.errors.push_back(listResp.is_object() ? listResp.value("error", std::string("card list failed"))
                                                : std::string("card list failed"));
        return r;
    }
    for (const auto& item : listResp.value("cards", nlohmann::json::array())) {
        if (!item.is_object()) continue;
        const std::string number = item.value("number", std::string());
        const std::string title = item.value("title", std::string());
        nlohmann::json showReq = {{"action", "card_show"},
                                  {"id", item.value("directionId", std::string())},
                                  {"taskId", taskId}};
        nlohmann::json showResp;
        std::string body;
        if (send(showReq, showResp, err) && showResp.is_object() && showResp.value("ok", false) &&
            showResp.contains("card") && showResp["card"].is_object()) {
            body = showResp["card"].value("body", std::string());
        }
        nlohmann::json editReq = {{"action", "card_edit"}, {"taskId", taskId}, {"number", number}};
        bool changed = false;
        std::string out;
        std::string state;
        if (IsNonEnglishText(title) && !CardFieldExcluded("card.title", translateCards)) {
            if (!TranslateGetOne(title, "en", "card.title", send, out, state, err)) {
                r.errors.push_back(number + ": " + err);
            } else if (state == "translated") {
                editReq["title"] = out;
                changed = true;
            } else if (state != "original") {
                ++r.pending;
            }
        }
        if (!body.empty() && IsNonEnglishText(body) && !CardFieldExcluded("card.body", translateCards)) {
            if (!TranslateGetOne(body, "en", "card.body", send, out, state, err)) {
                r.errors.push_back(number + ": " + err);
            } else if (state == "translated") {
                editReq["body"] = out;
                changed = true;
            } else if (state != "original") {
                ++r.pending;
            }
        }
        if (!changed) continue;
        nlohmann::json editResp;
        if (!send(editReq, editResp, err)) {
            r.errors.push_back(number + ": " + err);
            continue;
        }
        if (!editResp.is_object() || !editResp.value("ok", false)) {
            r.errors.push_back(number + ": " +
                               (editResp.is_object() ? editResp.value("error", std::string("failed"))
                                                     : std::string("failed")));
            continue;
        }
        ++r.updated;
        r.actions.push_back(CardMdTr(lang, "英語に訳した ", "translated ", "traducido ") + number);
    }
    r.ok = r.errors.empty();
    return r;
}

nlohmann::json CardMarkdownImportResultJson(const CardMarkdownImportResult& result) {
    return nlohmann::json{
        {"ok", result.ok},
        {"dryRun", result.dryRun},
        {"added", result.added},
        {"updated", result.updated},
        {"skipped", result.skipped},
        {"actions", result.actions},
        {"warnings", result.warnings},
        {"errors", result.errors}};
}

} // namespace pc
