/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "llmhistory.h"

#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace fcitx::chinesellm;

namespace {
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "testllmhistory: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

bool validDisplayUtf8(const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset++]);
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            continue;
        }
        size_t remaining = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1; value = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2; value = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3; value = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > text.size() - offset) return false;
        while (remaining-- > 0) {
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f)) return false;
    }
    return true;
}

FetchResult successful() {
    FetchResult result;
    result.seq = 77;
    result.ok = true;
    result.started = true;
    result.elapsedMs = 1234;
    result.httpStatus = 200;
    result.pinyin = "nihao";
    result.content = "你好\n您好";
    result.candidates = {"你好", "您好"};
    result.rejected = {{"wrong", "han_length_mismatch"}};
    return result;
}

// Status transitions are the history's contract with the addon.
void lifecycleTests() {
    RequestHistory history;
    const auto id = history.begin("nihao", "model-one");
    check(id != 0 && history.find(id)->status == RequestStatus::Queued,
          "a new request is queued");
    history.markStarted(id);
    check(history.find(id)->status == RequestStatus::InFlight &&
              history.find(id)->started, "started request is in flight");

    auto result = successful();
    history.finish(id, result);
    const auto *entry = history.find(id);
    check(entry->status == RequestStatus::Completed && entry->seq == 77 &&
              entry->elapsedMs == 1234 && entry->httpStatus == 200 &&
              entry->candidates == result.candidates &&
              entry->rejected.size() == 1, "finish records outcome and diagnostics");
    check(!history.adoptable(id), "an unshown reply cannot be adopted");
    history.markUsable(id, true);
    check(entry->status == RequestStatus::Usable && entry->overlap == true &&
              history.adoptable(id), "a shown reply is usable and adoptable");
    history.adopt(id, "different", false);
    check(entry->adoption.empty(), "only an exact candidate can be adopted");
    history.adopt(id, "你好", false);
    check(entry->adoption == "llm" && entry->adoptedText == "你好" &&
              !history.adoptable(id), "exact adoption is recorded once");

    const auto stale = history.begin("nihao", "m");
    history.finish(stale, result);
    history.markStale(stale);
    check(history.find(stale)->status == RequestStatus::Stale, "stale reply is marked");

    // The real cause of a cancellation survives the worker's generic report.
    const auto cancelled = history.begin("nihao", "m");
    history.markCancelled(cancelled, "focus_lost");
    FetchResult aborted;
    aborted.cancelled = true;
    aborted.error = "cancelled";
    history.finish(cancelled, aborted);
    check(history.find(cancelled)->status == RequestStatus::Cancelled &&
              history.find(cancelled)->error == "focus_lost",
          "cancellation keeps its specific reason");
    history.markCancelled(id, "late");
    check(history.find(id)->status == RequestStatus::Usable,
          "a finished request cannot be cancelled afterwards");

    FetchResult failed;
    failed.error = "http 500";
    history.finish(stale, failed);
    check(history.find(stale)->status == RequestStatus::Error, "failure is an error");
    FetchResult empty;
    empty.ok = true;
    history.finish(stale, empty);
    check(history.find(stale)->status == RequestStatus::Empty, "no output is empty");
    empty.rejected = {{"x", "invalid_text"}};
    history.finish(stale, empty);
    check(history.find(stale)->status == RequestStatus::Filtered,
          "only rejected output is filtered");

    const auto hit = history.recordCacheHit("nihao", "m", {"你好"}, false);
    check(history.find(hit)->status == RequestStatus::CacheHit &&
              history.find(hit)->overlap == false && history.adoptable(hit),
          "cache hits are recorded without a request");
    history.adopt(hit, "你好", true);
    check(history.find(hit)->adoption == "native", "cache hit can be adopted");
}

// IDs outlive eviction and clearing, so a late event can never resurrect or
// corrupt another record.
void identityTests() {
    RequestHistory history;
    const auto first = history.begin("nihao", "m");
    for (int i = 0; i < 100; ++i) history.begin("pinyin", "m");
    check(history.entries().size() == 100 && !history.find(first),
          "oldest entry is evicted at 100");
    history.markStarted(first);
    history.finish(first, successful());
    history.adopt(first, "你好", false);
    check(history.entries().size() == 100 && !history.find(first),
          "events for an evicted entry are ignored");
    const auto last = history.entries().back().id;
    history.clear();
    history.finish(last, successful());
    check(history.entries().empty(), "events after clear are ignored");
    const auto fresh = history.begin("new", "m");
    check(fresh > last, "clearing never reuses ids");
    history.adopt(fresh, "你好", false);
    check(history.find(fresh)->adoption.empty(), "old identities cannot leak into new entries");
}

void safetyAndLimitTests() {
    RequestHistory history;
    const auto id = history.begin("nihao", "model");
    auto result = successful();
    std::string chinese;
    for (int i = 0; i < 4000; ++i) chinese += "你";
    result.content = chinese;
    result.error = chinese;
    result.candidates.assign(40, chinese);
    result.rejected.assign(40, RejectedCandidate{chinese, chinese});
    history.finish(id, result);
    const auto *entry = history.find(id);
    check(entry->content.size() <= 8192 && contains(entry->content, "…"),
          "response is bounded with a truncation marker");
    check(entry->candidates.size() == 32 && entry->rejected.size() == 32,
          "accepted and rejected lists are capped");
    for (const auto &candidate : entry->candidates) {
        check(candidate.size() <= 1024 && validDisplayUtf8(candidate),
              "displayed text is bounded and valid UTF-8");
    }
    history.markUsable(id, false);
    history.adopt(id, entry->candidates.front(), false);
    check(entry->adoption.empty(), "truncated text cannot impersonate the original");
    history.adopt(id, chinese, false);
    check(entry->adoption == "llm" && entry->adoptedText.size() <= 1024,
          "the exact long original still adopts, displayed bounded");

    // Native duplicates can push a late candidate onto the screen.
    result.candidates.assign(32, "native-duplicate");
    result.candidates.push_back("novel-33rd");
    history.finish(id, result);
    history.markUsable(id, false);
    history.adopt(id, "novel-33rd", false);
    check(entry->adoption == "llm", "a candidate beyond the displayed 32 can be adopted");

    const std::string malicious = std::string("A\0B\n\r\t", 7) + "\x01\x7f\x80\xff" +
                                  "\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80" +
                                  "\xc2\x85\xe2\x80\xae" + "正常";
    result.content = malicious;
    result.error = malicious;
    result.candidates = {malicious};
    result.rejected = {{malicious, "x"}};
    history.finish(id, result);
    check(validDisplayUtf8(entry->content) && validDisplayUtf8(entry->error) &&
              validDisplayUtf8(entry->candidates.front()),
          "invalid UTF-8 and controls never reach the viewer");
    check(contains(entry->content, "\\x00") && contains(entry->content, "\\u202E") &&
              contains(entry->content, "正常"),
          "unsafe bytes become visible escapes while valid text is kept");
}

void rendererTests() {
    RequestHistory history;
    auto html = renderHistoryHtml(history, false, HistoryLanguage::English);
    check(contains(html, "No request history") && contains(html, "Disabled"),
          "empty history explains itself");
    check(contains(html, "default-src 'none'") && !contains(html, "<script") &&
              !contains(html, "href=") && !contains(html, "src="),
          "page is script-free with a restrictive CSP");
    history.begin("old-pinyin", "old-model");
    const std::string attack = "<script>alert('x')</script><img src=x onerror=alert(1)>\"&";
    const auto second = history.begin("new-pinyin" + attack, "model" + attack);
    auto result = successful();
    result.content = attack;
    result.error = attack;
    result.candidates = {attack};
    result.rejected = {{attack, attack}};
    history.finish(second, result);
    history.markUsable(second, true);
    history.adopt(second, attack, false);
    html = renderHistoryHtml(history, true, HistoryLanguage::English);
    check(html.find("new-pinyin") < html.find("old-pinyin"), "newest request first");
    check(contains(html, "Usable") && contains(html, "Seq 77") &&
              contains(html, "HTTP 200") && contains(html, "LLM candidate adopted") &&
              contains(html, "Rejected (1)"),
          "page shows status, timing, adoption and rejections");
    check(contains(html, "&lt;script&gt;") && contains(html, "&quot;&amp;") &&
              !contains(html, "<script>") && !contains(html, "<img"),
          "every untrusted field is HTML-escaped");

    // One language per page: Chinese by default, never both at once.
    RequestHistory sample;
    const auto cancelled = sample.begin("ni'hao", "m");
    sample.markCancelled(cancelled, "input_keys_or_context_changed");
    const auto chinese = renderHistoryHtml(sample, true);
    check(contains(chinese, "已取消") && contains(chinese, "按键或上文已变化") &&
              contains(chinese, "title=\"input_keys_or_context_changed\"") &&
              !contains(chinese, "Cancelled") && !contains(chinese, "Pinyin") &&
              !contains(chinese, " / Request"),
          "default page is Chinese only, with the raw code kept as a tooltip");
    check(!contains(chinese, "class=\"text error\""),
          "a cancellation is not displayed as an error");
    const auto english = renderHistoryHtml(sample, true, HistoryLanguage::English);
    check(contains(english, "Cancelled") && contains(english, "Keys or context changed") &&
              !contains(english, "已取消"),
          "English page contains no Chinese labels");
    check(historyLanguageFromLocale("zh_CN.UTF-8") == HistoryLanguage::Chinese &&
              historyLanguageFromLocale("zh_TW:en_US") == HistoryLanguage::Chinese &&
              historyLanguageFromLocale("C") == HistoryLanguage::Chinese &&
              historyLanguageFromLocale("en_US.UTF-8") == HistoryLanguage::English &&
              historyLanguageFromLocale("de_DE.UTF-8") == HistoryLanguage::English,
          "locale picks the page language, Chinese first");
    RequestHistory failed;
    const auto failedId = failed.begin("x", "m");
    auto failure = successful();
    failure.ok = false;
    failure.error = "http 500";
    failed.finish(failedId, failure);
    check(contains(renderHistoryHtml(failed, true), "class=\"text error\""),
          "a real failure is still displayed as an error");
}

std::string readSnapshot(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "snapshot readable by owner");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct RuntimeEnvironment {
    bool hadValue = std::getenv("XDG_RUNTIME_DIR") != nullptr;
    std::string oldValue = hadValue ? std::getenv("XDG_RUNTIME_DIR") : "";
    ~RuntimeEnvironment() {
        if (hadValue) ::setenv("XDG_RUNTIME_DIR", oldValue.c_str(), 1);
        else ::unsetenv("XDG_RUNTIME_DIR");
    }
};

void snapshotTests() {
    RuntimeEnvironment environment;
    char runtimePattern[] = "/tmp/test-fcitx-llm-history-XXXXXX";
    const char *created = ::mkdtemp(runtimePattern);
    check(created != nullptr, "create test runtime directory");
    const std::string runtime = created;
    check(runtime[0] == '/', "test runtime path is absolute");
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    std::string savedPath;
    std::string savedDirectory;
    {
        HistorySnapshot snapshot;
        check(snapshot.path().empty() && !snapshot.exists(), "default snapshot does not write disk");
        const auto oldMask = ::umask(0);
        const bool written = snapshot.write("<html>first</html>");
        ::umask(oldMask);
        check(written && snapshot.exists(), "explicit snapshot writes");
        savedPath = snapshot.path();
        savedDirectory = savedPath.substr(0, savedPath.rfind('/'));
        check(savedDirectory.compare(0, runtime.size() + 1, runtime + "/") == 0, "valid XDG runtime directory used");
        struct stat fileInfo {}, directoryInfo {};
        check(::lstat(savedPath.c_str(), &fileInfo) == 0 && S_ISREG(fileInfo.st_mode) &&
                  fileInfo.st_uid == ::geteuid() && (fileInfo.st_mode & 0777) == 0600, "snapshot exactly 0600 even with permissive umask");
        check(::lstat(savedDirectory.c_str(), &directoryInfo) == 0 && S_ISDIR(directoryInfo.st_mode) &&
                  directoryInfo.st_uid == ::geteuid() && (directoryInfo.st_mode & 0777) == 0700, "snapshot directory exactly 0700");
        check(readSnapshot(savedPath) == "<html>first</html>", "snapshot exact bytes");
        check(snapshot.write("<html>second</html>") && snapshot.path() == savedPath &&
                  readSnapshot(savedPath) == "<html>second</html>", "atomic rewrite keeps path and replaces bytes");
        struct stat replaced {};
        check(::lstat(savedPath.c_str(), &replaced) == 0 && replaced.st_ino != fileInfo.st_ino, "rewrite uses a new inode");

        // Both paths are inside our freshly-created test runtime directory.
        const std::string sentinelPath = runtime + "/sentinel";
        { std::ofstream sentinel(sentinelPath); sentinel << "must not change"; }
        check(::unlink(savedPath.c_str()) == 0 &&
                  ::symlink(sentinelPath.c_str(), savedPath.c_str()) == 0, "place hostile snapshot symlink");
        check(!snapshot.exists(), "exists rejects symlinks");
        check(snapshot.write("safe replacement") && readSnapshot(sentinelPath) == "must not change" &&
                  readSnapshot(savedPath) == "safe replacement", "write replaces symlink without following");
        DIR *directory = ::opendir(savedDirectory.c_str());
        check(directory != nullptr, "inspect owned snapshot directory");
        size_t files = 0;
        while (const auto *item = ::readdir(directory)) {
            if (std::string(item->d_name) != "." && std::string(item->d_name) != "..") ++files;
        }
        ::closedir(directory);
        check(files == 1, "no temporary files remain after writes");
        snapshot.clear();
        check(snapshot.path().empty() && !snapshot.exists() &&
                  ::lstat(savedDirectory.c_str(), &directoryInfo) != 0 && errno == ENOENT, "clear removes own snapshot and directory");
        check(readSnapshot(sentinelPath) == "must not change", "clear leaves unrelated files intact");
        snapshot.clear();
        check(snapshot.write("after clear"), "snapshot can be reopened after clear");
        savedPath = snapshot.path();
        savedDirectory = savedPath.substr(0, savedPath.rfind('/'));
        check(::unlink(sentinelPath.c_str()) == 0, "remove owned sentinel");
    }
    struct stat info {};
    check(::lstat(savedPath.c_str(), &info) != 0 && errno == ENOENT &&
              ::lstat(savedDirectory.c_str(), &info) != 0 && errno == ENOENT, "destructor removes snapshot and directory");
    for (const char *invalidRuntime : {"relative-path", "/definitely-not-a-real-fcitx-runtime-directory"}) {
        ::setenv("XDG_RUNTIME_DIR", invalidRuntime, 1);
        HistorySnapshot snapshot;
        check(snapshot.write("fallback") && snapshot.path().compare(0, 5, "/tmp/") == 0, "invalid XDG runtime falls back to /tmp");
    }
    if (::geteuid() != 0) {
        ::setenv("XDG_RUNTIME_DIR", "/", 1);
        HistorySnapshot snapshot;
        check(snapshot.write("fallback") && snapshot.path().compare(0, 5, "/tmp/") == 0,
              "runtime directory owned by another user is rejected");
    }
    const std::string symlinkRuntime = runtime + "/runtime-link";
    check(::symlink(runtime.c_str(), symlinkRuntime.c_str()) == 0, "create owned runtime symlink");
    ::setenv("XDG_RUNTIME_DIR", symlinkRuntime.c_str(), 1);
    {
        HistorySnapshot snapshot;
        check(snapshot.write("fallback") && snapshot.path().compare(0, runtime.size(), runtime) != 0, "symlink XDG runtime rejected");
    }
    check(::unlink(symlinkRuntime.c_str()) == 0, "remove owned runtime symlink");
    check(::rmdir(runtime.c_str()) == 0, "all test runtime contents cleaned up");
}
} // namespace

int main() {
    lifecycleTests();
    identityTests();
    safetyAndLimitTests();
    rendererTests();
    snapshotTests();
    return EXIT_SUCCESS;
}
