#pragma once

#include <cstddef>
#include <string>

// Small, dependency-free parsers for the timestamp formats Hancho reads.
// They replace sscanf so the MSVC C4996 warning does not hide real problems.
namespace pc {

namespace parse_detail {

inline bool ReadInt(const char*& p, const char* end, int& out) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    bool negative = false;
    if (p < end && (*p == '+' || *p == '-')) {
        negative = (*p == '-');
        ++p;
    }
    const char* start = p;
    long long value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 1000000000LL) value = 1000000000LL;
        ++p;
    }
    if (p == start) return false;
    out = static_cast<int>(negative ? -value : value);
    return true;
}

inline bool ReadChar(const char*& p, const char* end, char& out) {
    if (p >= end) return false;
    out = *p++;
    return true;
}

} // namespace parse_detail

// Parses "YYYY-MM-DD[ T:_]HH:MM:SS" and ignores any fraction/zone suffix.
inline bool ParseDateTimeFields(const std::string& s, int& year, int& month, int& day,
                                int& hour, int& minute, int& second) {
    using namespace parse_detail;
    const char* p = s.c_str();
    const char* end = p + s.size();
    char sep = 0;
    if (!ReadInt(p, end, year)) return false;
    if (!ReadChar(p, end, sep) || sep != '-') return false;
    if (!ReadInt(p, end, month)) return false;
    if (!ReadChar(p, end, sep) || sep != '-') return false;
    if (!ReadInt(p, end, day)) return false;
    if (!ReadChar(p, end, sep)) return false;
    if (sep != ' ' && sep != 'T' && sep != 't' && sep != '_' && sep != ':') return false;
    if (!ReadInt(p, end, hour)) return false;
    if (!ReadChar(p, end, sep) || sep != ':') return false;
    if (!ReadInt(p, end, minute)) return false;
    if (!ReadChar(p, end, sep) || sep != ':') return false;
    if (!ReadInt(p, end, second)) return false;
    return true;
}

// Parses "YYYY-MM-DD".
inline bool ParseDateFields(const std::string& s, int& year, int& month, int& day) {
    using namespace parse_detail;
    const char* p = s.c_str();
    const char* end = p + s.size();
    char sep = 0;
    if (!ReadInt(p, end, year)) return false;
    if (!ReadChar(p, end, sep) || sep != '-') return false;
    if (!ReadInt(p, end, month)) return false;
    if (!ReadChar(p, end, sep) || sep != '-') return false;
    if (!ReadInt(p, end, day)) return false;
    return true;
}

// Parses "YYYY-MM-DDTHH:MM:SS[.fff]Z" into fields plus milliseconds.
inline bool ParseIsoUtcFields(const std::string& s, int& year, int& month, int& day,
                              int& hour, int& minute, int& second, int& millis) {
    if (!ParseDateTimeFields(s, year, month, day, hour, minute, second)) return false;
    millis = 0;
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos) return true;
    const size_t dot = s.find('.', colon);
    if (dot == std::string::npos) return true;
    int value = 0;
    int digits = 0;
    size_t i = dot + 1;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3) {
        value = value * 10 + (s[i] - '0');
        ++i;
        ++digits;
    }
    while (digits < 3) {
        value *= 10;
        ++digits;
    }
    millis = value;
    return true;
}

// Parses "YYYY-MM".
inline bool ParseYearMonthFields(const std::string& s, int& year, int& month) {
    using namespace parse_detail;
    const char* p = s.c_str();
    const char* end = p + s.size();
    char sep = 0;
    if (!ReadInt(p, end, year)) return false;
    if (!ReadChar(p, end, sep) || sep != '-') return false;
    if (!ReadInt(p, end, month)) return false;
    return true;
}

} // namespace pc
