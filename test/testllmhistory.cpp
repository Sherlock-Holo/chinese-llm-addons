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
    result.content = "[\"你好\",\"您好\"]";
    result.candidates = {"你好", "您好"};
    result.rejected = {{"wrong", "pinyin mismatch"}};
    return result;
}

void historyTests() {
    RequestHistory history;
    check(history.entries().empty(), "history starts empty");
    const auto first = history.begin("nihao", "model-one");
    check(first != 0, "identity is nonzero");
    check(history.find(first)->status == "queued", "begin is queued");
    check(history.find(first)->timestampMs > 0, "begin timestamps request");
    const RequestHistory &constant = history;
    check(constant.find(first) != nullptr && !constant.find(0), "const find works");
    auto result = successful();
    history.complete(first, result);
    auto *entry = history.find(first);
    check(entry->pinyin == "nihao" && entry->model == "model-one", "begin metadata retained");
    check(entry->seq == 77 && entry->started && entry->elapsedMs == 1234 &&
              entry->httpStatus == 200, "completion copies timing/status fields");
    check(entry->content == result.content && entry->error.empty() &&
              entry->candidates == result.candidates && entry->rejected.size() == 1 &&
              entry->rejected.front().reason == "pinyin mismatch", "completion copies diagnostics");
    check(entry->status == "completed" && !entry->overlap.has_value(), "completion status/unknown overlap");
    history.adopt(first, "different", false);
    check(entry->adoption.empty() && entry->adoptedText.empty(), "different candidate is never adopted");
    history.adopt(first, "你好", false);
    check(entry->adoption == "llm" && entry->adoptedText == "你好", "exact LLM adoption");
    history.adopt(first, "您好", true);
    check(entry->adoption == "native" && entry->adoptedText == "您好", "exact native adoption");
    history.adopt(first, "invalid", false);
    check(entry->adoption == "native" && entry->adoptedText == "您好", "mismatch does not overwrite adoption");

    for (int i = 0; i < 100; ++i) history.begin("pinyin", "model");
    check(history.entries().size() == 100 && !history.find(first), "evicts oldest at 100");
    history.complete(first, result);
    history.adopt(first, "你好", false);
    check(history.entries().size() == 100 && !history.find(first), "evicted callback cannot recreate");
    const auto lastId = history.entries().back().id;
    history.clear();
    check(history.entries().empty() && !history.find(lastId), "clear drops records");
    history.complete(lastId, result);
    history.adopt(lastId, "你好", false);
    check(history.entries().empty(), "late completion/adoption after clear ignored");
    const auto newId = history.begin("new", "model");
    check(newId > lastId, "clear never resets IDs");
    history.adopt(newId, "你好", false);
    check(history.find(newId)->adoption.empty(), "old identities cannot leak into new entry");

    result = {};
    result.cancelled = true;
    history.complete(newId, result);
    check(history.find(newId)->status == "cancelled", "cancelled wins over error");
    result.cancelled = false;
    result.error = "network error";
    history.complete(newId, result);
    check(history.find(newId)->status == "error", "failed fetch is error");
    result.ok = true;
    result.error.clear();
    result.content = " \t\r\n";
    history.complete(newId, result);
    check(history.find(newId)->status == "empty", "blank successful body is empty");
    result.content = "not accepted";
    history.complete(newId, result);
    check(history.find(newId)->status == "empty", "success without candidates or rejections is empty");
    result.content = "[]";
    history.complete(newId, result);
    check(history.find(newId)->status == "empty", "empty candidate array is empty, not filtered");
    result.content.clear();
    result.rejected = {{"bad", "not a string"}};
    history.complete(newId, result);
    check(history.find(newId)->status == "filtered", "rejection even without raw body is filtered");
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
    history.complete(id, result);
    auto *entry = history.find(id);
    check(entry->content.size() <= 8192 && contains(entry->content, "…"), "response bounded with truncation marker");
    check(entry->error.size() <= 1024 && validDisplayUtf8(entry->error), "error UTF-8-safe limit");
    check(entry->candidates.size() == 32 && entry->rejected.size() == 32, "accepted and rejected count caps");
    for (const auto &candidate : entry->candidates) {
        check(candidate.size() <= 1024 && validDisplayUtf8(candidate) &&
                  contains(candidate, "…"), "candidate UTF-8-safe truncation");
    }
    for (const auto &candidate : entry->rejected) {
        check(candidate.text.size() <= 1024 && candidate.reason.size() <= 1024 &&
                  validDisplayUtf8(candidate.text) && validDisplayUtf8(candidate.reason), "rejected text/reason caps");
    }
    history.adopt(id, entry->candidates.front(), false);
    check(entry->adoption.empty(), "truncated text cannot impersonate exact original");
    history.adopt(id, chinese, false);
    check(entry->adoption == "llm" && entry->adoptedText.size() <= 1024 &&
              contains(entry->adoptedText, "…"), "long original matches, adoption display remains bounded");

    result.candidates = {std::string(65537, 'x'), "allowed"};
    history.complete(id, result);
    entry->adoption.clear();
    entry->adoptedText.clear();
    history.adopt(id, result.candidates[0], false);
    check(entry->adoption.empty(), "candidate over identity budget cannot adopt");
    history.adopt(id, "allowed", false);
    check(entry->adoption == "llm", "smaller candidate within remaining budget can adopt");
    result.candidates = {std::string(40000, 'a'), std::string(40000, 'b')};
    history.complete(id, result);
    entry->adoption.clear();
    history.adopt(id, result.candidates[1], false);
    check(entry->adoption.empty(), "aggregate exact identity bytes capped at 64 KiB");
    result.candidates.assign(32, "native-duplicate");
    result.candidates.push_back("novel-33rd");
    history.complete(id, result);
    check(entry->candidates.size() == 32, "public candidate display stays capped at 32");
    history.adopt(id, "novel-33rd", false);
    check(entry->adoption == "llm" && entry->adoptedText == "novel-33rd",
          "33rd exact candidate can adopt after native duplicates are skipped");
    result.candidates.assign(40, "");
    result.candidates.push_back("after-empty");
    history.complete(id, result);
    history.adopt(id, "", false);
    check(entry->adoption.empty(), "empty candidate identities are never retained or adopted");
    history.adopt(id, "after-empty", false);
    check(entry->adoption == "llm" && entry->adoptedText == "after-empty",
          "empty candidates do not prevent later exact adoption");

    const std::string malicious = std::string("A\0B\n\r\t", 7) + "\x01\x7f\x80\xff" +
                                  "\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80" +
                                  "\xc2\x85\xe2\x80\xae" + "正常";
    result.content = malicious;
    result.error = malicious;
    result.candidates = {malicious};
    result.rejected = {{malicious, "non-string: <object>\n"}};
    history.complete(id, result);
    check(validDisplayUtf8(entry->content) && validDisplayUtf8(entry->error) &&
              validDisplayUtf8(entry->candidates.front()), "invalid UTF-8 and controls sanitized");
    check(contains(entry->content, "\\x00") && contains(entry->content, "\\n") &&
              contains(entry->content, "\\xFF") && contains(entry->content, "\\u0085") &&
              contains(entry->content, "\\u202E") && contains(entry->content, "正常"), "invalid/control bytes visible ASCII, valid Chinese kept");
    history.adopt(id, entry->candidates.front(), false);
    check(entry->adoption.empty(), "escaped display cannot impersonate raw exact candidate");
    history.adopt(id, malicious, false);
    check(entry->adoption == "llm" && validDisplayUtf8(entry->adoptedText), "raw exact adoption sanitized for display");
}

void rendererTests() {
    RequestHistory history;
    auto html = renderHistoryHtml(history, false);
    check(contains(html, "<!doctype html>") && contains(html, "No request history") &&
              contains(html, "Disabled") && contains(html, "Clear LLM request history"), "standalone empty/disabled/clear instructions");
    check(contains(html, "default-src 'none'") && contains(html, "style-src 'unsafe-inline'"), "restrictive CSP");
    check(!contains(html, "<script") && !contains(html, "<a ") &&
              !contains(html, "src=") && !contains(html, "href="), "no scripts, resources, or links");
    const auto first = history.begin("old-pinyin", "old-model");
    const std::string attack = "<script>alert('x')</script><img src=x onerror=alert(1)>\"&";
    const auto second = history.begin("new-pinyin" + attack, "model" + attack);
    auto result = successful();
    result.content = attack;
    result.error = attack;
    result.candidates = {attack};
    result.rejected = {{attack, "not a string: " + attack}};
    history.complete(second, result);
    auto *entry = history.find(second);
    entry->timestampMs = 123456789;
    entry->status = "usable";
    entry->overlap = true;
    history.adopt(second, attack, false);
    html = renderHistoryHtml(history, true);
    check(html.find("new-pinyin") < html.find("old-pinyin"), "newest rendered first");
    check(contains(html, "Enabled") && contains(html, "Usable") && contains(html, "123456789 ms") &&
              contains(html, "Seq 77") && contains(html, "1234 ms") && contains(html, "HTTP 200") &&
              contains(html, "LLM adoption") && contains(html, "Native overlap"), "renderer shows metadata, overlap and adoption");
    check(contains(html, "Raw LLM") && contains(html, "Accepted (1)") &&
              contains(html, "Rejected &amp; why (1)") && contains(html, "not a string:"), "all diagnostic fields included");
    check(contains(html, "&lt;script&gt;") && contains(html, "&#39;x&#39;") &&
              contains(html, "&quot;&amp;") && !contains(html, "<script>") &&
              !contains(html, "<img"), "all untrusted fields escaped against XSS");
    entry->status = attack;
    entry->adoption = attack;
    entry->pinyin = std::string("p\0\xff", 3);
    html = renderHistoryHtml(history, true);
    check(!contains(html, "<script>") && contains(html, "\\x00\\xFF"), "mutable public fields revalidated/escaped");
    entry->status = "stale";
    entry->overlap = false;
    history.adopt(second, attack, true);
    html = renderHistoryHtml(history, true);
    check(contains(html, "Stale") && contains(html, "Native adoption") &&
              contains(html, "HTTP 200</td><td>否 / No<br>"), "stale, false overlap and native adoption labels");
    entry->overlap = true;
    html = renderHistoryHtml(history, true);
    check(contains(html, "HTTP 200</td><td>是 / Yes<br>"), "true overlap rendered distinctly");
    history.find(first)->status = "cancelled";
    html = renderHistoryHtml(history, true);
    check(contains(html, "Cancelled") && contains(html, "Unknown"), "cancelled and unknown overlap labels");
    const std::pair<const char *, const char *> statuses[] = {
        {"queued", "Queued"}, {"inflight", "请求中 / In flight"},
        {"cache_hit", "缓存命中 / Cache hit"}, {"error", "Error"}, {"empty", "Empty"},
        {"filtered", "Filtered"}, {"completed", "Completed"}};
    for (const auto &status : statuses) {
        entry->status = status.first;
        html = renderHistoryHtml(history, true);
        check(contains(html, status.second), "fixed status has bilingual label");
    }
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
    historyTests();
    safetyAndLimitTests();
    rendererTests();
    snapshotTests();
    return EXIT_SUCCESS;
}
