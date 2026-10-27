/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "llmhistory.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fcitx::chinesellm {
namespace {
constexpr size_t MaxEntries = 100;
constexpr size_t MaxContentBytes = 8 * 1024;
constexpr size_t MaxTextBytes = 1024;
constexpr size_t MaxCandidates = 32;
constexpr size_t MaxIdentityBytes = 64 * 1024;
constexpr char SnapshotName[] = "history.html";
constexpr char Hex[] = "0123456789ABCDEF";

std::string byteEscape(unsigned char byte) {
    std::string result = "\\x00";
    result[2] = Hex[byte >> 4];
    result[3] = Hex[byte & 15];
    return result;
}

// Return one complete UTF-8 character, or a visible ASCII escape for malformed
// bytes and controls. Never pass embedded NULs/invalid UTF-8 to UI or D-Bus.
std::string displayToken(const std::string &input, size_t &offset) {
    const auto first = static_cast<unsigned char>(input[offset]);
    if (first < 0x80) {
        ++offset;
        if (first == '\n') {
            return "\\n";
        }
        if (first == '\r') {
            return "\\r";
        }
        if (first == '\t') {
            return "\\t";
        }
        if (first < 0x20 || first == 0x7f) {
            return byteEscape(first);
        }
        return std::string(1, static_cast<char>(first));
    }
    size_t length = 0;
    uint32_t value = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
        length = 2;
        value = first & 0x1f;
        minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
        length = 3;
        value = first & 0x0f;
        minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
        length = 4;
        value = first & 0x07;
        minimum = 0x10000;
    }
    bool valid = length != 0 && length <= input.size() - offset;
    for (size_t i = 1; valid && i < length; ++i) {
        const auto next = static_cast<unsigned char>(input[offset + i]);
        if ((next & 0xc0) != 0x80) {
            valid = false;
        } else {
            value = (value << 6) | (next & 0x3f);
        }
    }
    valid = valid && value >= minimum && value <= 0x10ffff &&
            !(value >= 0xd800 && value <= 0xdfff);
    if (!valid) {
        ++offset;
        return byteEscape(first);
    }
    const auto start = offset;
    offset += length;
    // Escape C1 and directionality controls too: these must not disguise
    // rejected candidates or HTML source in the viewer.
    if ((value >= 0x80 && value <= 0x9f) || value == 0x061c ||
        value == 0x200e || value == 0x200f ||
        (value >= 0x202a && value <= 0x202e) ||
        (value >= 0x2066 && value <= 0x2069) || value == 0xfeff) {
        std::string result = "\\u0000";
        for (size_t i = 0; i < 4; ++i) {
            result[5 - i] = Hex[(value >> (i * 4)) & 15];
        }
        return result;
    }
    return input.substr(start, length);
}

std::string displayText(const std::string &input, size_t limit) {
    std::string result;
    std::vector<size_t> boundaries;
    result.reserve(std::min(input.size(), limit));
    size_t offset = 0;
    while (offset < input.size()) {
        auto token = displayToken(input, offset);
        if (token.size() > limit - result.size()) {
            constexpr char Ellipsis[] = "\xe2\x80\xa6";
            if (limit >= 3) {
                while (!boundaries.empty() && result.size() > limit - 3) {
                    result.resize(boundaries.back());
                    boundaries.pop_back();
                }
                result += Ellipsis;
            }
            break;
        }
        boundaries.push_back(result.size());
        result += token;
    }
    return result;
}

std::string escapeHtml(const std::string &input) {
    // Revalidate even public entry fields which the main-thread caller may edit.
    const auto safe = displayText(input, std::numeric_limits<size_t>::max());
    std::string result;
    for (char character : safe) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += character; break;
        }
    }
    return result;
}

void wipe(std::string &text) {
    volatile char *data = text.empty() ? nullptr : text.data();
    for (size_t i = 0; i < text.size(); ++i) {
        data[i] = 0;
    }
    std::string().swap(text);
}

void wipe(std::vector<std::string> &texts) {
    for (auto &text : texts) {
        wipe(text);
    }
    std::vector<std::string>().swap(texts);
}

void wipe(RequestHistoryEntry &entry) {
    wipe(entry.pinyin);
    wipe(entry.model);
        wipe(entry.content);
    wipe(entry.error);
    wipe(entry.adoption);
    wipe(entry.adoptedText);
    wipe(entry.candidates);
    for (auto &candidate : entry.rejected) {
        wipe(candidate.text);
        wipe(candidate.reason);
    }
    std::vector<RejectedCandidate>().swap(entry.rejected);
}

std::string statusLabel(RequestStatus status) {
    switch (status) {
    case RequestStatus::Queued: return "排队 / Queued";
    case RequestStatus::InFlight: return "请求中 / In flight";
    case RequestStatus::CacheHit: return "缓存命中 / Cache hit";
    case RequestStatus::Cancelled: return "已取消 / Cancelled";
    case RequestStatus::Error: return "错误 / Error";
    case RequestStatus::Empty: return "空响应 / Empty";
    case RequestStatus::Filtered: return "全部被拒绝 / Filtered";
    case RequestStatus::Completed: return "已完成 / Completed";
    case RequestStatus::Usable: return "可用 / Usable";
    case RequestStatus::Stale: return "已过期 / Stale";
    }
    return "";
}

bool validRuntimeDirectory(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    struct stat info {};
    if (::lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) ||
        info.st_uid != ::geteuid()) {
        return false;
    }
    const int fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat opened {};
    const bool valid = ::fstat(fd, &opened) == 0 && S_ISDIR(opened.st_mode) &&
                       opened.st_uid == ::geteuid() &&
                       opened.st_dev == info.st_dev && opened.st_ino == info.st_ino;
    ::close(fd);
    return valid;
}
} // namespace

RequestHistory::~RequestHistory() { clear(); }

uint64_t RequestHistory::begin(std::string pinyin, std::string model) {
    if (nextId_ == std::numeric_limits<uint64_t>::max()) {
        return 0; // Never wrap and reuse a callback identity.
    }
    RequestHistoryEntry entry;
    entry.id = ++nextId_;
    entry.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    entry.pinyin = displayText(pinyin, MaxTextBytes);
    entry.model = displayText(model, MaxTextBytes);
    entries_.push_back(std::move(entry));
    if (entries_.size() > MaxEntries) {
        const auto oldestId = entries_.front().id;
        const auto identities = candidateIdentities_.find(oldestId);
        if (identities != candidateIdentities_.end()) {
            wipe(identities->second);
            candidateIdentities_.erase(identities);
        }
        wipe(entries_.front());
        entries_.pop_front();
    }
    return nextId_;
}

RequestHistoryEntry *RequestHistory::mutableEntry(uint64_t id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [id](const auto &entry) { return entry.id == id; });
    return found == entries_.end() ? nullptr : &*found;
}

const RequestHistoryEntry *RequestHistory::find(uint64_t id) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
        [id](const auto &entry) { return entry.id == id; });
    return found == entries_.end() ? nullptr : &*found;
}

uint64_t RequestHistory::recordCacheHit(
    std::string pinyin, std::string model,
    const std::vector<std::string> &candidates, bool overlap) {
    const auto id = begin(std::move(pinyin), std::move(model));
    auto *entry = mutableEntry(id);
    if (!entry) {
        return 0;
    }
    for (size_t i = 0; i < std::min(candidates.size(), MaxCandidates); ++i) {
        entry->candidates.push_back(displayText(candidates[i], MaxTextBytes));
    }
    setIdentities(id, candidates);
    entry->status = RequestStatus::CacheHit;
    entry->overlap = overlap;
    return id;
}

void RequestHistory::setSeq(uint64_t id, uint64_t seq) {
    if (auto *entry = mutableEntry(id)) {
        entry->seq = seq;
    }
}

void RequestHistory::markStarted(uint64_t id) {
    if (auto *entry = mutableEntry(id)) {
        entry->started = true;
        if (entry->status == RequestStatus::Queued) {
            entry->status = RequestStatus::InFlight;
        }
    }
}

void RequestHistory::markCancelled(uint64_t id, std::string reason) {
    auto *entry = mutableEntry(id);
    if (entry && (entry->status == RequestStatus::Queued ||
                  entry->status == RequestStatus::InFlight)) {
        entry->status = RequestStatus::Cancelled;
        wipe(entry->error);
        entry->error = displayText(reason, MaxTextBytes);
    }
}

void RequestHistory::markUsable(uint64_t id, bool overlap) {
    if (auto *entry = mutableEntry(id)) {
        entry->status = RequestStatus::Usable;
        entry->overlap = overlap;
    }
}

void RequestHistory::markStale(uint64_t id) {
    if (auto *entry = mutableEntry(id)) {
        entry->status = RequestStatus::Stale;
        wipe(entry->error);
        entry->error = "Reply no longer matches the current input";
    }
}

bool RequestHistory::adoptable(uint64_t id) const {
    const auto *entry = find(id);
    return entry && entry->adoption.empty() &&
           (entry->status == RequestStatus::Usable ||
            entry->status == RequestStatus::CacheHit);
}

void RequestHistory::setIdentities(uint64_t id,
                                   const std::vector<std::string> &candidates) {
    auto &identities = candidateIdentities_[id];
    wipe(identities);
    // Display truncation must not suppress adoption tracking: native duplicate
    // removal can make the 33rd (or later) LLM candidate visible in the UI.
    size_t identityBytes = 0;
    for (const auto &text : candidates) {
        if (!text.empty() && text.size() <= MaxIdentityBytes - identityBytes) {
            identities.push_back(text);
            identityBytes += text.size();
        }
        if (identityBytes == MaxIdentityBytes) {
            break;
        }
    }
}

void RequestHistory::finish(uint64_t id, const FetchResult &result) {
    auto *entry = mutableEntry(id);
    if (!entry) {
        return;
    }
    // A cancellation recorded by the owner names the real cause; keep it.
    std::string reason;
    if (result.cancelled && entry->status == RequestStatus::Cancelled) {
        reason = entry->error;
    }
    entry->seq = result.seq;
    entry->started = result.started;
    entry->elapsedMs = result.elapsedMs;
    entry->httpStatus = result.httpStatus;
    wipe(entry->content);
    wipe(entry->error);
    wipe(entry->adoption);
    wipe(entry->adoptedText);
    entry->overlap.reset();
    entry->content = displayText(result.content, MaxContentBytes);
    entry->error = displayText(result.error, MaxTextBytes);
    wipe(entry->candidates);
    for (auto &rejected : entry->rejected) {
        wipe(rejected.text);
        wipe(rejected.reason);
    }
    entry->rejected.clear();
    for (size_t i = 0; i < std::min(result.candidates.size(), MaxCandidates); ++i) {
        entry->candidates.push_back(displayText(result.candidates[i], MaxTextBytes));
    }
    setIdentities(id, result.candidates);
    for (size_t i = 0; i < std::min(result.rejected.size(), MaxCandidates); ++i) {
        const auto &candidate = result.rejected[i];
        entry->rejected.push_back({displayText(candidate.text, MaxTextBytes),
                                  displayText(candidate.reason, MaxTextBytes)});
    }
    if (result.cancelled) {
        entry->status = RequestStatus::Cancelled;
        if (!reason.empty()) {
            wipe(entry->error);
            entry->error = std::move(reason);
        }
    } else if (!result.ok) {
        entry->status = RequestStatus::Error;
    } else if (!entry->candidates.empty()) {
        entry->status = RequestStatus::Completed;
    } else if (!result.rejected.empty()) {
        entry->status = RequestStatus::Filtered;
    } else {
        entry->status = RequestStatus::Empty;
    }
}

void RequestHistory::adopt(uint64_t id, std::string text, bool native) {
    auto *entry = mutableEntry(id);
    const auto identities = candidateIdentities_.find(id);
    if (!entry || identities == candidateIdentities_.end() ||
        std::find(identities->second.begin(), identities->second.end(), text) ==
            identities->second.end()) {
        return;
    }
    entry->adoption = native ? "native" : "llm";
    wipe(entry->adoptedText);
    entry->adoptedText = displayText(text, MaxTextBytes);
}

void RequestHistory::clear() {
    for (auto &entry : entries_) {
        wipe(entry);
    }
    for (auto &identities : candidateIdentities_) {
        wipe(identities.second);
    }
    std::deque<RequestHistoryEntry>().swap(entries_);
    decltype(candidateIdentities_)().swap(candidateIdentities_);
}

std::string renderHistoryHtml(const RequestHistory &history, bool enabled) {
    std::ostringstream html;
    html << "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
         << "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
            "style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'\">"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         << "<title>LLM 请求历史 / Request history</title><style>"
         << ":root{color-scheme:light dark;font-family:system-ui,sans-serif}"
            "body{margin:0;padding:24px;background:#f5f7fb;color:#202938}"
            "main{max-width:1500px;margin:auto}h1{font-size:1.6rem;margin:0 0 12px}"
            ".note{line-height:1.7;color:#526074}.state{font-weight:600}"
            ".scroll{overflow:auto;max-height:75vh;border:1px solid #ccd4df;"
            "border-radius:10px;background:white}table{border-collapse:collapse;"
            "width:100%;min-width:1000px}th,td{text-align:left;vertical-align:top;"
            "padding:12px;border-bottom:1px solid #dfe5ee}th{position:sticky;top:0;"
            "background:#eaf0f8;z-index:1;font-size:.9rem}td{font-size:.9rem}"
            "tr:nth-child(even){background:#f9fbfe}small{color:#526074}"
            "pre,.text{white-space:pre-wrap;overflow-wrap:anywhere;word-break:break-word}"
            "pre{max-height:20rem;overflow:auto;border:1px solid #ccd4df;padding:10px;"
            "border-radius:6px;min-width:260px;font-size:.85rem}"
            "details{margin:8px 0}summary{cursor:pointer;font-weight:600}"
            "ol{padding-left:24px;max-height:20rem;overflow:auto}li{margin:7px 0}"
            ".error{color:#b42318}.empty{padding:24px;border:1px dashed #a9b7cb;"
            "border-radius:10px}@media(prefers-color-scheme:dark){body{background:#151a22;"
            "color:#e5edf7}.scroll{background:#202734;border-color:#445164}"
            "th{background:#303d50}tr:nth-child(even){background:#252e3c}"
            "th,td,pre{border-color:#445164}.note,small{color:#aab9cc}.error{color:#ffaaa3}}"
         << "</style></head><body><main><h1>LLM 请求历史 / Request history</h1>"
         << "<p class=\"state\">LLM："
         << (enabled ? "已启用 / Enabled" : "已关闭 / Disabled")
         << " · " << history.entries().size() << " / 100</p>"
         << "<p class=\"note\">仅保留最近 100 次请求，不保存上下文或 API 密钥。"
            "响应显示最多 8 KiB，候选与拒绝项各最多 32 条，每项显示最多 1024 字节；"
            "截断以 … 标记。<br>Last 100 requests only; no context or API keys. "
            "Response display: 8 KiB; accepted/rejected: 32 each; text display: "
            "1024 bytes each. … indicates truncation.<br>此页面是只读本地快照，"
            "刷新浏览器可查看已更新的快照。请使用输入法状态菜单“清空 LLM 请求历史”清空记录；"
            "页面本身不能清空。关闭浏览器不会清空内存。<br>Read-only local snapshot; "
            "refresh the browser to view updates. Use the input-method status menu "
            "“Clear LLM request history” to clear records. This page cannot clear history; "
            "closing the browser does not clear memory.</p>";
    if (history.entries().empty()) {
        html << "<p class=\"empty\">暂无请求历史 / No request history. "
                "启用 LLM 后输入拼音可产生记录。 / Enable LLM and type pinyin to create records.</p>";
    } else {
        html << "<div class=\"scroll\"><table><thead><tr>"
                "<th>时间 / Time<br>ID · Seq</th><th>拼音 / Pinyin<br>模型 / Model</th>"
                "<th>状态 / Status<br>请求已开始 / Started</th>"
                "<th>耗时 / Elapsed<br>HTTP</th><th>原生重合 / Native overlap<br>采纳 / Adoption</th>"
                "<th>响应与候选 / Response &amp; candidates</th></tr></thead><tbody>";
        for (auto iter = history.entries().rbegin(); iter != history.entries().rend(); ++iter) {
            const auto &entry = *iter;
            html << "<tr><td>" << entry.timestampMs << " ms<br><small>ID " << entry.id
                 << " · Seq " << entry.seq << "</small></td><td class=\"text\">"
                 << escapeHtml(entry.pinyin) << "<br><small>" << escapeHtml(entry.model)
                 << "</small></td><td>" << statusLabel(entry.status) << "<br><small>"
                 << (entry.started ? "是 / Yes" : "否 / No") << "</small>";
            if (!entry.error.empty()) {
                html << "<p class=\"text error\">错误 / Error: " << escapeHtml(entry.error)
                     << "</p>";
            }
            html << "</td><td>" << entry.elapsedMs << " ms<br>HTTP " << entry.httpStatus
                 << "</td><td>";
            if (!entry.overlap.has_value()) {
                html << "未知 / Unknown";
            } else {
                html << (*entry.overlap ? "是 / Yes" : "否 / No");
            }
            html << "<br>";
            if (entry.adoption == "native") {
                html << "原生候选采纳 / Native adoption";
            } else if (entry.adoption == "llm") {
                html << "LLM 候选采纳 / LLM adoption";
            } else if (entry.adoption.empty()) {
                html << "未采纳 / Not adopted";
            } else {
                html << escapeHtml(entry.adoption);
            }
            if (!entry.adoptedText.empty()) {
                html << "<p class=\"text\">" << escapeHtml(entry.adoptedText) << "</p>";
            }
            html << "</td><td><details><summary>原始 LLM / Raw LLM</summary><pre>"
                 << (entry.content.empty() ? "（无响应） / No response" : escapeHtml(entry.content))
                 << "</pre></details><details><summary>接受的候选 / Accepted ("
                 << entry.candidates.size() << ")</summary>";
            if (entry.candidates.empty()) {
                html << "<p>无 / None</p>";
            } else {
                html << "<ol>";
                for (const auto &candidate : entry.candidates) {
                    html << "<li class=\"text\">" << escapeHtml(candidate) << "</li>";
                }
                html << "</ol>";
            }
            html << "</details><details><summary>拒绝的候选与原因 / Rejected &amp; why ("
                 << entry.rejected.size() << ")</summary>";
            if (entry.rejected.empty()) {
                html << "<p>无 / None</p>";
            } else {
                html << "<ol>";
                for (const auto &candidate : entry.rejected) {
                    html << "<li class=\"text\">" << escapeHtml(candidate.text)
                         << "<br><small>原因 / Reason: " << escapeHtml(candidate.reason)
                         << "</small></li>";
                }
                html << "</ol>";
            }
            html << "</details></td></tr>";
        }
        html << "</tbody></table></div>";
    }
    html << "</main></body></html>";
    return html.str();
}

HistorySnapshot::~HistorySnapshot() { clear(); }

bool HistorySnapshot::ensureDirectory() {
    if (directoryFd_ >= 0) {
        return true;
    }
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string base = validRuntimeDirectory(runtime) ? runtime : "/tmp";
    std::string pattern = base + "/fcitx-llm-history-XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    if (!::mkdtemp(path.data())) {
        return false;
    }
    directory_ = path.data();
    directoryFd_ = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info {};
    if (directoryFd_ < 0 || ::fstat(directoryFd_, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
        ::fchmod(directoryFd_, 0700) != 0) {
        clear();
        return false;
    }
    return true;
}

bool HistorySnapshot::write(const std::string &html) {
    if (!ensureDirectory()) {
        return false;
    }
    std::string temporary;
    int fd = -1;
    for (size_t attempt = 0; attempt < 128; ++attempt) {
        temporary = ".snapshot-" + std::to_string(::getpid()) + "-" +
                    std::to_string(++temporaryId_) + ".tmp";
        fd = ::openat(directoryFd_, temporary.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t offset = 0;
    while (ok && offset < html.size()) {
        const size_t count = std::min(html.size() - offset,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::write(fd, html.data() + offset, count);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ok = false;
        } else {
            offset += static_cast<size_t>(written);
        }
    }
    // No fsync: the snapshot is a disposable view regenerated from memory, and
    // a synchronous flush would stall the input thread on slow disks.
    if (::close(fd) != 0) {
        ok = false;
    }
    if (ok) {
        // renameat replaces a symlink itself; it never follows the destination.
        ok = ::renameat(directoryFd_, temporary.c_str(), directoryFd_, SnapshotName) == 0;
    }
    if (!ok) {
        ::unlinkat(directoryFd_, temporary.c_str(), 0);
    }
    return ok;
}

bool HistorySnapshot::exists() const {
    struct stat info {};
    return directoryFd_ >= 0 &&
           ::fstatat(directoryFd_, SnapshotName, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
           (info.st_mode & 0777) == 0600;
}

void HistorySnapshot::clear() {
    if (directoryFd_ >= 0) {
        ::unlinkat(directoryFd_, SnapshotName, 0);
        ::close(directoryFd_);
        directoryFd_ = -1;
    }
    if (!directory_.empty()) {
        ::rmdir(directory_.c_str());
        directory_.clear();
    }
}

} // namespace fcitx::chinesellm
