#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace htest {

struct CheckRecord {
    bool ok = false;
    std::string label;
    std::string detail;
};

class TestContext {
public:
    explicit TestContext(std::string id) : id_(std::move(id)) {}

    const std::string& Id() const { return id_; }

    void Check(bool ok, const std::string& label, const std::string& detail = "") {
        records_.push_back({ok, label, detail});
    }

    void Note(const std::string& detail) { notes_.push_back(detail); }

    int Failures() const {
        int n = 0;
        for (const auto& r : records_) {
            if (!r.ok) ++n;
        }
        return n;
    }

    const std::vector<CheckRecord>& Records() const { return records_; }
    const std::vector<std::string>& Notes() const { return notes_; }

private:
    std::string id_;
    std::vector<CheckRecord> records_;
    std::vector<std::string> notes_;
};

using TestFn = std::function<void(TestContext&)>;

struct TestEntry {
    std::string id;
    TestFn fn;
};

inline std::vector<TestEntry>& Registry() {
    static std::vector<TestEntry> entries;
    return entries;
}

struct Registrar {
    Registrar(const char* id, TestFn fn) {
        Registry().push_back({id, std::move(fn)});
    }
};

#define HT_CAT2(a, b) a##b
#define HT_CAT(a, b) HT_CAT2(a, b)

#define HANCHO_TEST(id, ...) \
    static ::htest::Registrar HT_CAT(htest_entry_, __LINE__)(id, __VA_ARGS__)

#define HT_CHECK(ctx, cond, label) (ctx).Check((cond), (label))
#define HT_CHECK_D(ctx, cond, label, detail) (ctx).Check((cond), (label), (detail))

} // namespace htest
