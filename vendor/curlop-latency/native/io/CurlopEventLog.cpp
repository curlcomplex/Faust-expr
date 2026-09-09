#include "io/CurlopEventLog.h"

#include <chrono>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace CurlopEventLog {

namespace {

std::mutex& mutex() { static std::mutex m; return m; }
std::FILE*& fileHandle() { static std::FILE* f = nullptr; return f; }

constexpr std::size_t kRealtimeCapacity = 2048;
constexpr std::size_t kRealtimeFlagBytes = 32;
constexpr std::size_t kRealtimeMsgBytes = 768;

struct RealtimeEvent {
    std::atomic<uint32_t> ready { 0 };
    char flag[kRealtimeFlagBytes] {};
    char msg[kRealtimeMsgBytes] {};
};

std::array<RealtimeEvent, kRealtimeCapacity>& realtimeQueue()
{
    static std::array<RealtimeEvent, kRealtimeCapacity> q;
    return q;
}

std::atomic<uint32_t>& realtimeWriteIndex() { static std::atomic<uint32_t> i { 0 }; return i; }
std::atomic<uint32_t>& realtimeReadIndex()  { static std::atomic<uint32_t> i { 0 }; return i; }

void copyBounded(char* dst, std::size_t dstBytes, const char* src) noexcept
{
    if (dstBytes == 0)
        return;
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, dstBytes, "%s", src);
}

// Append-escape one UTF-8 string into out as a JSON string body (no enclosing
// quotes). Handles ", \, control chars per RFC 8259.
void appendEscaped(std::string& out, const char* s, std::size_t len) {
    out.reserve(out.size() + len + 8);
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

double getNowSeconds() {
    using namespace std::chrono;
    const auto t = system_clock::now().time_since_epoch();
    return duration_cast<duration<double>>(t).count();
}

} // namespace

void init(const char* path) {
    std::lock_guard<std::mutex> lock(mutex());
    if (fileHandle() != nullptr) return;  // already open

    // Priority: explicit path arg > CURLOP_EVENT_LOG_PATH env >
    //           CURLOP_SESSION_ID env (→ /tmp/curlop-events.<sid>.jsonl) >
    //           /tmp/curlop-events.latest.jsonl (single-instance default).
    std::string sessionPath;
    const char* resolved = path;
    if (resolved == nullptr) {
        resolved = std::getenv("CURLOP_EVENT_LOG_PATH");
    }
    if (resolved == nullptr) {
        const char* sid = std::getenv("CURLOP_SESSION_ID");
        if (sid != nullptr && *sid != '\0') {
            sessionPath = std::string("/tmp/curlop-events.") + sid + ".jsonl";
            resolved = sessionPath.c_str();
        } else {
            resolved = "/tmp/curlop-events.latest.jsonl";
        }
    }

    std::FILE* f = std::fopen(resolved, "a");  // append, create if missing
    if (f == nullptr) {
        std::fprintf(stderr, "[CurlopEventLog] failed to open %s: %s\n",
                     resolved, std::strerror(errno));
        return;
    }
    std::setvbuf(f, nullptr, _IOLBF, 0);  // line-buffered
    fileHandle() = f;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(mutex());
    if (fileHandle() != nullptr) {
        std::fclose(fileHandle());
        fileHandle() = nullptr;
    }
}

void emit(const char* flag, const char* msg) {
    std::lock_guard<std::mutex> lock(mutex());
    std::FILE* f = fileHandle();
    if (f == nullptr) return;

    std::string line;
    line.reserve(64 + (msg ? std::strlen(msg) : 0));
    char tsBuf[32];
    std::snprintf(tsBuf, sizeof(tsBuf), "%.3f", getNowSeconds());
    line += "{\"ts\":";
    line += tsBuf;
    line += ",\"source\":\"cpp\",\"flag\":\"";
    line += (flag ? flag : "");
    line += "\",\"msg\":\"";
    if (msg != nullptr) appendEscaped(line, msg, std::strlen(msg));
    line += "\"}\n";

    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
}

bool emitRealtime(const char* flag, const char* msg) noexcept
{
    auto& write = realtimeWriteIndex();
    auto& read = realtimeReadIndex();
    uint32_t w = write.load(std::memory_order_relaxed);
    for (;;)
    {
        const uint32_t r = read.load(std::memory_order_acquire);
        if (static_cast<uint32_t>(w - r) >= kRealtimeCapacity)
            return false;
        if (write.compare_exchange_weak(w, w + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
            break;
    }

    auto& event = realtimeQueue()[w % kRealtimeCapacity];
    copyBounded(event.flag, sizeof(event.flag), flag);
    copyBounded(event.msg, sizeof(event.msg), msg);
    event.ready.store(w + 1, std::memory_order_release);
    return true;
}

void drainRealtime()
{
    auto& write = realtimeWriteIndex();
    auto& read = realtimeReadIndex();
    uint32_t r = read.load(std::memory_order_relaxed);
    const uint32_t w = write.load(std::memory_order_acquire);
    while (r != w)
    {
        auto& event = realtimeQueue()[r % kRealtimeCapacity];
        if (event.ready.load(std::memory_order_acquire) != r + 1)
            break;
        emit(event.flag, event.msg);
        event.ready.store(0, std::memory_order_release);
        ++r;
    }
    read.store(r, std::memory_order_release);
}

void emitSnapshot(const char* kind, const char* payload, std::size_t len) {
    std::lock_guard<std::mutex> lock(mutex());
    std::FILE* f = fileHandle();
    if (f == nullptr) return;

    // Heuristic: if payload doesn't start with { [ " digit / -, treat as
    // string (escape it). Otherwise treat as raw JSON value.
    const bool looksLikeJson = (len > 0) && (
        payload[0] == '{' || payload[0] == '[' || payload[0] == '"' ||
        payload[0] == '-' || (payload[0] >= '0' && payload[0] <= '9') ||
        std::strncmp(payload, "true", 4) == 0 ||
        std::strncmp(payload, "false", 5) == 0 ||
        std::strncmp(payload, "null", 4) == 0);

    std::string line;
    line.reserve(64 + len);
    char tsBuf[32];
    std::snprintf(tsBuf, sizeof(tsBuf), "%.3f", getNowSeconds());
    line += "{\"ts\":";
    line += tsBuf;
    line += ",\"source\":\"snapshot\",\"kind\":\"";
    line += (kind ? kind : "");
    line += "\",\"payload\":";
    if (looksLikeJson) {
        line.append(payload, len);
    } else {
        line += '"';
        appendEscaped(line, payload, len);
        line += '"';
    }
    line += "}\n";

    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
}

} // namespace CurlopEventLog
