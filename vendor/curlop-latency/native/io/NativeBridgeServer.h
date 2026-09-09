#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// NativeBridgeServer — Lightweight WebSocket server for the MCP transport
//
// Post-T-164 (s375) the WebView is gone, but this server stays: it is the
// transport that the external MCP server (native/mcp-server/index.mjs) uses
// to reach the running CURLOP process. Runs on a background thread bound
// to 127.0.0.1 on an ephemeral port; clients connect via ws://127.0.0.1:{port}
// and exchange binary frames with magic-header framing.
//
// Binary protocol (all little-endian):
//   BYTE (0x42595445) — bytecode program
//   DCMD (0x44434D44) — diagnostic/MCP command
//   PSET/PRMI/TRNS are retired; parameter mutation enters through DCMD
//   USER_SET_PARAM so persistence, diagnostics, and request correlation stay
//   on the same authority path.
//
// Thread safety:
//   - WebSocket I/O runs on a dedicated std::thread (no juce:: types)
//   - Cross-thread communication via std::atomic and std::mutex queues
//   - Program swap uses the existing AtomicProgramSwap (lock-free)
//
// Modeled on the legacy MidiIngestServer.h.hazmat pattern but using POSIX
// sockets directly instead of CHOC HTTPServer (which requires Boost).
// ═══════════════════════════════════════════════════════════════════════════

#include "shell/BootEmits.h"      // s427 — emit DEBUG_FLAGS_STATE on flag change
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <deque>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 using curlop_socket_t = SOCKET;
 using curlop_socklen_t = int;
 using pollfd = WSAPOLLFD;
 #ifndef ssize_t
  using ssize_t = SSIZE_T;
 #endif
#else
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <arpa/inet.h>
 #include <unistd.h>
 #include <poll.h>
 #include <fcntl.h>
 using curlop_socket_t = int;
 using curlop_socklen_t = socklen_t;
#endif

#include "shell/CurlopDebug.h"

namespace curlop {

inline curlop_socket_t invalidSocket() noexcept
{
#if JUCE_WINDOWS
    return INVALID_SOCKET;
#else
    return -1;
#endif
}

inline bool isValidSocket(curlop_socket_t fd) noexcept
{
    return fd != invalidSocket();
}

inline void closeSocket(curlop_socket_t fd) noexcept
{
#if JUCE_WINDOWS
    closesocket(fd);
#else
    ::close(fd);
#endif
}

inline void shutdownSocket(curlop_socket_t fd) noexcept
{
#if JUCE_WINDOWS
    ::shutdown(fd, SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

inline bool lastSocketErrorIsAddressInUse() noexcept
{
#if JUCE_WINDOWS
    return WSAGetLastError() == WSAEADDRINUSE;
#else
    return errno == EADDRINUSE;
#endif
}

inline bool lastSocketErrorIsWouldBlock() noexcept
{
#if JUCE_WINDOWS
    const auto error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

inline void initSockets()
{
#if JUCE_WINDOWS
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void) started;
#endif
}

inline int socketPoll(pollfd* fds, int nfds, int timeoutMs)
{
#if JUCE_WINDOWS
    return WSAPoll(fds, static_cast<ULONG>(nfds), timeoutMs);
#else
    return poll(fds, nfds, timeoutMs);
#endif
}

inline ssize_t socketRecv(curlop_socket_t fd, void* data, size_t size) noexcept
{
#if JUCE_WINDOWS
    return ::recv(fd, static_cast<char*>(data), static_cast<int>(size), 0);
#else
    return ::recv(fd, data, size, 0);
#endif
}

inline ssize_t socketSend(curlop_socket_t fd, const void* data, size_t size) noexcept
{
#if JUCE_WINDOWS
    return ::send(fd, static_cast<const char*>(data), static_cast<int>(size), 0);
#else
    return ::send(fd, data, size, 0);
#endif
}

inline long long socketLogValue(curlop_socket_t fd) noexcept
{
    return static_cast<long long>(fd);
}

// ═══════════════════════════════════════════════════════════════
// Magic headers for binary protocol
// ═══════════════════════════════════════════════════════════════
static constexpr uint32_t MAGIC_BYTE = 0x42595445; // 'BYTE'
static constexpr uint32_t MAGIC_DCMD = 0x44434D44; // 'DCMD'
static constexpr uint32_t MAGIC_MRSP = 0x4D525350; // 'MRSP' — MCP response
static constexpr uint32_t MAGIC_MIDI = 0x4D494449; // 'MIDI' — MIDI CC input data
// Tier 1 (0.C-1) Step 8: MAGIC_PRMI deleted. Bulk param seeding now flows via
// graphContainer_.paramValues (populated by applyPendingGraphParamsForClip at
// USER_LOAD_CLIP_GRAPH time), consumed by BYTE handler overlay (BridgeMessageHandler
// Step 3). [-r TRANSITIONAL (PRMI — died 0.C-1)]

// ═══════════════════════════════════════════════════════════════
// Parsed message types delivered to the processor
// ═══════════════════════════════════════════════════════════════

struct ParamMapping {
    uint32_t paramIdx = 0;
    uint32_t moduleIdx = 0;
    std::string paramName;
    float defaultValue = std::numeric_limits<float>::quiet_NaN();
};

struct LayoutDescriptor {
    uint32_t voices = 1;
    uint8_t steal  = 0;   // T-480: receiver steal policy (Delivery::StealPolicy)
    uint32_t inputs = 0;
    uint32_t outputs = 1;
    uint32_t declaredParamCount = 0;  // R-006: declared (non-system) params per module
    // SF-052 — {}-stacked per-voice base names (ALL_CAPS) for this module;
    // buildByteLayouts marks them per-voice so the declaration expands the rows.
    std::vector<std::string> perVoiceParams;
};

struct BytecodeMessage {
    std::vector<uint8_t> bytecode;
    uint32_t moduleCount = 0;
    uint16_t numGenerators = 0;  // T-278: per-program gen count, from CompileResult.
    std::vector<uint64_t> genKeys; // T-279 slice-1 (SPEC-013 §5.5): per-program
                                    // (gen_type, args_hash) keys parallel to gen_id.
                                    // Threaded CompileResult → meta JSON → here →
                                    // Program::genKeys. Slice-2 reads at install
                                    // time to migrate matching state.
    int32_t clipId = -1;
    uint64_t authorityGeneration = 0;
    // Project lifecycle generation captured when delayed work was authored.
    // This is local authority metadata, not part of the BYTE wire payload.
    uint64_t projectAuthorityEpoch = 0;
    std::vector<LayoutDescriptor> layouts;
    std::vector<ParamMapping> paramMappings;
    // Cut 4c: moduleNames + edges removed from BYTE wire — GraphSyncManager
    // owns graph topology via APG_GRAPH_STATE. C++ BYTE handler reads dsl
    // names from slot.moduleDslNames (populated by APG_GRAPH_STATE).

    // A legacy install-level transport intent remains for staged internal
    // work. User Play and Audition use ParamBufferCmd::START_TRANSPORT
    // instead: a rejected graph replacement must never cancel transport.
    // bpm is valid only when startTransport is true.
    bool  startTransport = false;
    float bpm            = 0.0f;
    // Zero is an unversioned legacy/internal start.  USER_PLAY_CLIP assigns a
    // monotonically increasing sequence so a later USER_STOP can cancel a
    // pending install before the audio thread applies it.
    uint64_t transportStartSequence = 0;
    uint64_t clipSwitchTraceId = 0;
};

struct ProjectAuthorityStamp {
    uint64_t epoch = 0;
    bool commitInProgress = false;
};

template <typename ReadEpoch, typename ReadCommit>
ProjectAuthorityStamp snapshotProjectAuthority(ReadEpoch readEpoch,
                                               ReadCommit readCommit)
{
    const auto epochBefore = readEpoch();
    const bool commitBefore = readCommit();
    const auto epochAfter = readEpoch();
    const bool commitAfter = readCommit();
    return { epochAfter,
             commitBefore || commitAfter || epochBefore != epochAfter };
}

struct DiagMessage {
    uint8_t subcommand;  // 0x10=MCP_COMMAND — the only live subcommand (T-335 retired 0x01/0x02 TraceFifo enable)
    std::string commandJson;  // only used when subcommand=0x10
    ProjectAuthorityStamp projectAuthority;
    // Diagnostic-only correlation for the bounded WebView performance request.
    // The command JSON remains the sole execution authority.
    std::string lifecycleRequestId;
};

enum class RequestLifecycleStage {
    Admission,
    Dequeue,
    HandlerEntry,
    SettlementRegistered,
    MrspEnqueue,
    OutboundDequeue,
    OutboundBackpressure,
    OutboundWrite,
    OutboundFailure
};

struct PendingRequestLifecycle {
    std::string requestId;
    bool backpressureRecorded = false;
};

template <typename EmitDisposition>
size_t settlePendingRequestLifecycles (
    std::deque<PendingRequestLifecycle>& pending,
    EmitDisposition emitDisposition)
{
    const auto count = pending.size();
    for (const auto& lifecycle : pending)
        emitDisposition (lifecycle.requestId);
    pending.clear();
    return count;
}

inline const char* requestLifecycleStageName (RequestLifecycleStage stage) noexcept
{
    switch (stage) {
        case RequestLifecycleStage::Admission:            return "admission";
        case RequestLifecycleStage::Dequeue:              return "dequeue";
        case RequestLifecycleStage::HandlerEntry:         return "handler_entry";
        case RequestLifecycleStage::SettlementRegistered: return "settlement_registered";
        case RequestLifecycleStage::MrspEnqueue:          return "mrsp_enqueue";
        case RequestLifecycleStage::OutboundDequeue:      return "outbound_dequeue";
        case RequestLifecycleStage::OutboundBackpressure: return "outbound_backpressure";
        case RequestLifecycleStage::OutboundWrite:        return "outbound_write";
        case RequestLifecycleStage::OutboundFailure:      return "outbound_failure";
    }
    return "unknown";
}

inline std::optional<std::string> readBoundedTraceJsonString (
    std::string_view json, std::string_view field)
{
    const auto key = std::string { "\"" } + std::string { field } + "\"";
    auto offset = json.find (key);
    if (offset == std::string_view::npos)
        return std::nullopt;
    offset = json.find (':', offset + key.size());
    if (offset == std::string_view::npos)
        return std::nullopt;
    ++offset;
    while (offset < json.size() && (json[offset] == ' ' || json[offset] == '\t'
                                    || json[offset] == '\r' || json[offset] == '\n'))
        ++offset;
    if (offset >= json.size() || json[offset++] != '"')
        return std::nullopt;

    std::string value;
    value.reserve (64);
    while (offset < json.size() && json[offset] != '"') {
        const auto c = json[offset++];
        if (value.size() >= 128 || c == '\\'
            || ! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '-' || c == '_'
                  || c == '.' || c == ':'))
            return std::nullopt;
        value.push_back (c);
    }
    return offset < json.size() && ! value.empty()
        ? std::optional<std::string> { std::move (value) } : std::nullopt;
}

inline bool isBoundedRequestLifecycleId (std::string_view value) noexcept
{
    if (value.empty() || value.size() > 128)
        return false;
    for (const auto c : value)
        if (! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '-' || c == '_'
               || c == '.' || c == ':'))
            return false;
    return true;
}

inline std::string readWebViewPerformanceLifecycleRequestId (std::string_view json)
{
    const auto command = readBoundedTraceJsonString (json, "cmd");
    const auto requestId = readBoundedTraceJsonString (json, "requestId");
    return command && *command == "webViewPerformance" && requestId
        ? *requestId : std::string {};
}

enum class WebSocketFrameParseStatus {
    NeedMoreData,
    Ready,
    ProtocolError
};

struct WebSocketFrameHeader {
    bool fin = false;
    bool masked = false;
    uint8_t opcode = 0;
    size_t headerLen = 0;
    size_t maskLen = 0;
    size_t payloadLen = 0;
    size_t totalLen = 0;
};

// Tier 1 (0.C-1) Step 8: ParamInitEntry + ParamInitMessage deleted. See
// MAGIC_PRMI comment above.

// ═══════════════════════════════════════════════════════════════
// Thread-safe message queue (mutex-based, main thread consumer)
// ═══════════════════════════════════════════════════════════════
template <typename T>
class MessageQueue {
public:
    void push(T&& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(msg));
    }

    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    size_t clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t count = queue_.size();
        queue_.clear();
        return count;
    }

private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
};

// ═══════════════════════════════════════════════════════════════
// NativeBridgeServer
// ═══════════════════════════════════════════════════════════════

class NativeBridgeServer {
public:
    enum class NonBlockingWriteState { Sent, WouldBlock, Failed };
    struct NonBlockingWriteAttempt {
        NonBlockingWriteState state = NonBlockingWriteState::Failed;
        size_t bytes = 0;
    };
    enum class PendingWriteStatus { Complete, Pending, Failed };
    using RequestLifecycleObserver = std::function<void (
        RequestLifecycleStage, const std::string&, const std::string&)>;

    NativeBridgeServer() {
        initSockets();
    }

    ~NativeBridgeServer() {
        stop();
    }

    // Non-copyable
    NativeBridgeServer(const NativeBridgeServer&) = delete;
    NativeBridgeServer& operator=(const NativeBridgeServer&) = delete;

    /// Start the server. Returns the bound port, or 0 on failure.
    /// desiredPort==0 → bind ephemeral immediately (skip 51120 contention).
    int start(int desiredPort = 51120) {
        if (running_.load()) return port_.load();

        serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (! isValidSocket(serverFd_)) return 0;

        // Allow port reuse
        int opt = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(static_cast<uint16_t>(desiredPort));

        if (::bind(serverFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            // EADDRINUSE fallback — another instance may be running
            if (lastSocketErrorIsAddressInUse()) {
                addr.sin_port = htons(0); // ephemeral fallback
                if (::bind(serverFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                    closeSocket(serverFd_);
                    serverFd_ = invalidSocket();
                    return 0;
                }
            } else {
                closeSocket(serverFd_);
                serverFd_ = invalidSocket();
                return 0;
            }
        }

        // Retrieve assigned port
        curlop_socklen_t addrLen = sizeof(addr);
        getsockname(serverFd_, reinterpret_cast<struct sockaddr*>(&addr), &addrLen);
        port_.store(ntohs(addr.sin_port));

        if (::listen(serverFd_, MAX_CLIENTS + 1) < 0) {
            closeSocket(serverFd_);
            serverFd_ = invalidSocket();
            return 0;
        }

        // Set non-blocking for the accept loop
        setNonBlocking(serverFd_);

        running_.store(true);
        serverThread_ = std::thread([this]() { runLoop(); });

        return port_.load();
    }

    /// Stop the server and close all connections.
    void stop() {
        running_.store(false);

        if (serverThread_.joinable()) {
            // Wake up poll by closing the server fd
            if (isValidSocket(serverFd_)) {
                shutdownSocket(serverFd_);
                closeSocket(serverFd_);
                serverFd_ = invalidSocket();
            }
            serverThread_.join();
        }
    }

    /// Get the bound port (0 if not started).
    int getPort() const { return port_.load(); }

    /// Check if the server is running.
    bool isRunning() const { return running_.load(); }

    void setRequestLifecycleObserverForTests (RequestLifecycleObserver observer)
    {
        requestLifecycleObserver_ = std::move (observer);
    }

    void traceRequestLifecycle (RequestLifecycleStage stage,
                                const std::string& requestId,
                                const std::string& detail = {})
    {
        if (! isBoundedRequestLifecycleId (requestId))
            return;
        if (! CurlopDebug::on (CurlopDebug::MCP_CMD)
            && ! requestLifecycleObserver_)
            return;
        auto boundedDetail = detail.substr (0, 80);
        for (auto& c : boundedDetail)
            if (! ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                   || (c >= '0' && c <= '9') || c == '-' || c == '_'
                   || c == '.' || c == ':'))
                c = '_';
        const auto atMs = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::system_clock::now().time_since_epoch()).count();
        CDBG(MCP_CMD, "REQUEST_LIFECYCLE requestId=%s stage=%s atMs=%lld detail=%s",
             requestId.c_str(), requestLifecycleStageName (stage),
             static_cast<long long> (atMs), boundedDetail.c_str());
        if (requestLifecycleObserver_)
            requestLifecycleObserver_ (stage, requestId, boundedDetail);
    }

    // ── Message queues (consumed by CurlopProcessor on main thread) ──

    MessageQueue<BytecodeMessage>  bytecodeQueue;
    MessageQueue<DiagMessage>      diagQueue;
    // Tier 1 (0.C-1) Step 8: paramInitQueue deleted (MAGIC_PRMI dies).
    // B-663: raw PSET inbound queue deleted; param mutation enters via DCMD
    // USER_SET_PARAM only.

    // Called by the bridge I/O thread after queueing inbound work that must be
    // handled on the JUCE message thread. The processor wires this to
    // AsyncUpdater so MCP commands do not depend solely on the audio-facing
    // processor timer being alive.
    void setInboundCallbacks(std::function<void()> queued,
                             std::function<ProjectAuthorityStamp()> authority)
    { std::lock_guard<std::mutex> lock(inboundCallbackMutex_); onInboundMessageQueued_ = std::move(queued); readProjectAuthorityAtIngress_ = std::move(authority); }
    void clearInboundCallbacks()
    { std::lock_guard<std::mutex> lock(inboundCallbackMutex_); onInboundMessageQueued_ = {}; readProjectAuthorityAtIngress_ = {}; }

    // Test-only lifecycle seam: exercises the same callback lifetime lock used
    // by DCMD dispatch without needing a live socket server.
    void invokeInboundCallbacksForTests()
    {
        std::lock_guard<std::mutex> lock(inboundCallbackMutex_);
        if (readProjectAuthorityAtIngress_)
            (void) readProjectAuthorityAtIngress_();
        if (onInboundMessageQueued_)
            onInboundMessageQueued_();
    }

    /// Queue a binary frame for broadcast to ALL connected clients.
    /// Thread-safe: can be called from any thread (typically timerCallback).
    /// The I/O thread will drain the queue and send WebSocket binary frames.
    void queueBroadcast(const uint8_t* data, size_t len) {
        OutboundMessage message;
        message.frame.assign (data, data + len);
        outQueue_.push(std::move(message));
    }

    /// Queue a binary frame with a magic header prefix.
    /// Convenience for the CURLOP binary protocol.
    void queueBroadcast(uint32_t magic, const uint8_t* payload, size_t payloadLen,
                        std::string lifecycleRequestId = {}) {
        OutboundMessage message;
        message.frame.resize (4 + payloadLen);
        std::memcpy(message.frame.data(), &magic, 4);
        if (payloadLen > 0)
            std::memcpy(message.frame.data() + 4, payload, payloadLen);
        message.lifecycleRequestId = std::move (lifecycleRequestId);
        outQueue_.push(std::move(message));
    }

    static bool isSupportedInboundMagic(uint32_t magic) {
        return magic == MAGIC_DCMD;
    }

    static std::vector<uint8_t> makeServerBinaryFrame(const uint8_t* data, size_t len)
    {
        std::vector<uint8_t> frame;
        frame.reserve(10 + len);
        frame.push_back(0x82); // FIN + binary opcode
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            const auto length = static_cast<uint64_t>(len);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((length >> (i * 8)) & 0xFF));
        }
        if (len > 0 && data != nullptr)
            frame.insert(frame.end(), data, data + len);
        return frame;
    }

    template <typename Send>
    static PendingWriteStatus drainPendingFrame(const std::vector<uint8_t>& frame,
                                                size_t& offset,
                                                Send&& send)
    {
        if (offset > frame.size()) return PendingWriteStatus::Failed;
        while (offset < frame.size()) {
            const auto attempt = send(frame.data() + offset, frame.size() - offset);
            if (attempt.state == NonBlockingWriteState::WouldBlock)
                return PendingWriteStatus::Pending;
            if (attempt.state != NonBlockingWriteState::Sent
                || attempt.bytes == 0 || attempt.bytes > frame.size() - offset)
                return PendingWriteStatus::Failed;
            offset += attempt.bytes;
        }
        return PendingWriteStatus::Complete;
    }

    static WebSocketFrameParseStatus parseClientWebSocketFrameHeader(const uint8_t* data,
                                                                     size_t bufSize,
                                                                     WebSocketFrameHeader& out)
    {
        out = {};
        if (data == nullptr) return WebSocketFrameParseStatus::ProtocolError;
        if (bufSize < 2) return WebSocketFrameParseStatus::NeedMoreData;

        out.fin = (data[0] & 0x80) != 0;
        out.opcode = data[0] & 0x0F;
        out.masked = (data[1] & 0x80) != 0;

        const bool isControlFrame = (out.opcode & 0x08) != 0;
        const bool supportedDataFrame = (out.opcode == 0x02);
        const bool supportedControlFrame = (out.opcode == 0x08 || out.opcode == 0x09 || out.opcode == 0x0A);
        if (! supportedDataFrame && ! supportedControlFrame)
            return WebSocketFrameParseStatus::ProtocolError;

        if (! out.masked)
            return WebSocketFrameParseStatus::ProtocolError;

        if (! out.fin)
            return WebSocketFrameParseStatus::ProtocolError;

        uint64_t payloadLen64 = data[1] & 0x7F;
        out.headerLen = 2;
        if (payloadLen64 == 126) {
            if (bufSize < 4) return WebSocketFrameParseStatus::NeedMoreData;
            payloadLen64 = (static_cast<uint64_t>(data[2]) << 8)
                         |  static_cast<uint64_t>(data[3]);
            out.headerLen = 4;
        } else if (payloadLen64 == 127) {
            if (bufSize < 10) return WebSocketFrameParseStatus::NeedMoreData;
            payloadLen64 = 0;
            for (int j = 0; j < 8; ++j)
                payloadLen64 = (payloadLen64 << 8) | static_cast<uint64_t>(data[2 + j]);
            out.headerLen = 10;
        }

        if (isControlFrame && payloadLen64 > 125)
            return WebSocketFrameParseStatus::ProtocolError;

        constexpr uint64_t maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
        if (payloadLen64 > maxSize)
            return WebSocketFrameParseStatus::ProtocolError;

        out.maskLen = 4;
        out.payloadLen = static_cast<size_t>(payloadLen64);
        if (out.payloadLen > std::numeric_limits<size_t>::max() - out.headerLen - out.maskLen)
            return WebSocketFrameParseStatus::ProtocolError;

        out.totalLen = out.headerLen + out.maskLen + out.payloadLen;
        if (bufSize < out.totalLen)
            return WebSocketFrameParseStatus::NeedMoreData;

        return WebSocketFrameParseStatus::Ready;
    }

    static bool parseBytecodePayload(const uint8_t* data, size_t len, BytecodeMessage& out) {
        out = {};
        if (data == nullptr || len < 5) return false;

        BytecodeMessage msg;
        size_t offset = 0;

        int32_t rawClipId;
        std::memcpy(&rawClipId, data + offset, 4);
        msg.clipId = rawClipId;
        offset += 4;

        msg.moduleCount = data[offset++];

        // Layout descriptors: voices(u8) + inputs(u8) + outputs(u8) + declaredParamCount(u8) per module
        for (uint32_t i = 0; i < msg.moduleCount; ++i) {
            if (offset + 4 > len) {
                fprintf(stderr, "[NativeBridge] BYTE: truncated layout descriptor at index %u\n", i);
                return false;  // Reject — layout/module count mismatch would corrupt VM state
            }
            LayoutDescriptor ld;
            ld.voices  = data[offset++];
            ld.inputs  = data[offset++];
            ld.outputs = data[offset++];
            ld.declaredParamCount = data[offset++];
            msg.layouts.push_back(ld);
        }

        if (msg.layouts.size() != msg.moduleCount) {
            fprintf(stderr, "[NativeBridge] BYTE: layout count mismatch (%zu parsed, %u expected)\n",
                msg.layouts.size(), msg.moduleCount);
            return false;
        }

        // Param mappings: paramCount(u16 LE) + [paramIdx(u8) + moduleIdx(u8) + default(f32 LE) + nameLen(u16 LE) + name(utf8)]...
        if (offset + 2 <= len) {
            uint16_t paramCount;
            std::memcpy(&paramCount, data + offset, 2);
            offset += 2;

            for (uint16_t i = 0; i < paramCount; ++i) {
                if (offset + 8 > len) {
                    fprintf(stderr, "[NativeBridge] BYTE: truncated param mapping header at index %d\n", i);
                    return false;  // Reject — partial param mappings corrupt index space
                }
                ParamMapping pm;
                pm.paramIdx = data[offset++];
                pm.moduleIdx = data[offset++];

                std::memcpy(&pm.defaultValue, data + offset, 4);
                offset += 4;
                if (! std::isfinite(pm.defaultValue)) {
                    fprintf(stderr, "[NativeBridge] BYTE: non-finite param default at index %d\n", i);
                    return false;
                }

                uint16_t nameLen;
                std::memcpy(&nameLen, data + offset, 2);
                offset += 2;

                if (offset + nameLen > len) {
                    fprintf(stderr, "[NativeBridge] BYTE: truncated param mapping name at index %d (need %d, have %zu)\n",
                        i, nameLen, len - offset);
                    return false;  // Reject entire message
                }
                pm.paramName.assign(
                    reinterpret_cast<const char*>(data + offset), nameLen);
                offset += nameLen;
                if (pm.paramName.empty()) {
                    fprintf(stderr, "[NativeBridge] BYTE: empty param mapping name at index %d\n", i);
                    return false;
                }

                if (pm.moduleIdx >= msg.moduleCount) {
                    fprintf(stderr, "[NativeBridge] BYTE: param mapping moduleIdx %u >= moduleCount %u, skipping\n",
                        pm.moduleIdx, msg.moduleCount);
                    continue;  // Skip this mapping but continue parsing
                }

                msg.paramMappings.push_back(std::move(pm));
            }
        }

        if (offset < len)
            msg.bytecode.assign(data + offset, data + len);

        out = std::move(msg);
        return true;
    }

    static bool parseDebugFlagsPayload(const uint8_t* data,
                                       size_t len,
                                       CurlopDebug::Mask current,
                                       CurlopDebug::Mask& out)
    {
        out = current;
        if (data == nullptr || data[0] != 0x30)
            return false;

        if (len != 9 && len != 17)
            return false;

        std::memcpy(&out.lo, data + 1, 8);
        if (len == 17)
            std::memcpy(&out.hi, data + 9, 8);

        return true;
    }

    static bool parseQueuedDiagPayload(const uint8_t* data, size_t len, DiagMessage& out)
    {
        out = {};
        if (data == nullptr || len < 2 || data[0] != 0x10)
            return false;

        out.subcommand = data[0];
        out.commandJson.assign(reinterpret_cast<const char*>(data + 1), len - 1);
        return ! out.commandJson.empty();
    }

    static bool applyQueuedDiagAuthority(
        DiagMessage& message,
        const std::function<ProjectAuthorityStamp()>& readAuthority)
    {
        if (! readAuthority)
            return false;
        const auto authority = readAuthority();
        if (authority.epoch == 0)
            return false;
        message.projectAuthority = authority;
        return true;
    }

private:
    struct OutboundMessage {
        std::vector<uint8_t> frame;
        std::string lifecycleRequestId;
    };

    std::atomic<bool> running_{ false };
    std::atomic<int>  port_{ 0 };
    curlop_socket_t serverFd_ = invalidSocket();
    std::thread serverThread_;
    // ── Outbound message queue (pushed by main thread, drained by I/O thread) ──
    // Thread-safe: timerCallback pushes, runLoop drains and sends.
    MessageQueue<OutboundMessage> outQueue_;
    RequestLifecycleObserver requestLifecycleObserver_;
    std::mutex inboundCallbackMutex_;
    std::function<void()> onInboundMessageQueued_;
    std::function<ProjectAuthorityStamp()> readProjectAuthorityAtIngress_;

    // ── Per-client state ──────────────────────────────────────
    // Support up to MAX_CLIENTS simultaneous WebSocket connections.
    // Slot 0 is typically the WebView. Additional slots for debug tools,
    // MCP server, external visualizers, etc.
    // Session 279 (B-122 followup): bumped 4 → 8. WebView internals consume
    // 2 slots (AudioBridge + bridge router); prior 4-slot budget left ~2 for
    // external clients, saturating when MCP server + T1 harness + dead-socket
    // TIME_WAIT collided during relaunch cycles. 8 slots fits all current
    // operational patterns with headroom.
    static constexpr int MAX_CLIENTS = 8;

    struct ClientState {
        curlop_socket_t fd = invalidSocket();
        bool upgraded = false;         // WebSocket handshake complete
        std::vector<uint8_t> rxBuf;    // Incomplete frame accumulator
        std::vector<uint8_t> txBuf;    // Complete frames awaiting nonblocking send
        size_t txOffset = 0;           // Exact next byte; retained across EAGAIN
        std::deque<PendingRequestLifecycle> pendingLifecycle;
        std::string httpBuf;           // HTTP request accumulator

        void reset() {
            if (isValidSocket(fd)) closeSocket(fd);
            fd = invalidSocket();
            upgraded = false;
            rxBuf.clear();
            txBuf.clear();
            txOffset = 0;
            pendingLifecycle.clear();
            httpBuf.clear();
        }

        bool isEmpty() const { return ! isValidSocket(fd); }
    };

    void retireClient (ClientState& client, const std::string& detail)
    {
        settlePendingRequestLifecycles (
            client.pendingLifecycle,
            [this, &detail] (const std::string& requestId)
            {
                traceRequestLifecycle (
                    RequestLifecycleStage::OutboundFailure, requestId, detail);
            });
        client.reset();
    }

    // ═══════════════════════════════════════════════════════════════
    // Server loop — runs on background thread
    // ═══════════════════════════════════════════════════════════════
    void runLoop() {
        ClientState clients[MAX_CLIENTS];

        while (running_.load()) {
            // ── Build pollfd array: server + all active clients ──
            struct pollfd fds[1 + MAX_CLIENTS];
            int fdToSlot[1 + MAX_CLIENTS]; // maps pollfd index -> client slot (-1 = server)
            int nfds = 0;

            // Always poll the server socket for new connections
            if (isValidSocket(serverFd_)) {
                fds[nfds].fd = serverFd_;
                fds[nfds].events = POLLIN;
                fdToSlot[nfds] = -1; // sentinel: this is the server
                nfds++;
            }

            // Poll each active client
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (isValidSocket(clients[i].fd)) {
                    fds[nfds].fd = clients[i].fd;
                    fds[nfds].events = static_cast<short> (POLLIN
                        | (clients[i].txOffset < clients[i].txBuf.size() ? POLLOUT : 0));
                    fdToSlot[nfds] = i;
                    nfds++;
                }
            }

            int ret = socketPoll(fds, nfds, 50); // 50ms timeout — responsive shutdown
            if (ret < 0) break;

            // ── Process poll results ──
            if (ret > 0) {
                for (int i = 0; i < nfds; ++i) {
                    if (fdToSlot[i] == -1) {
                        if (!(fds[i].revents & POLLIN)) continue;
                        // ── Accept new connection ──
                        struct sockaddr_in clientAddr{};
                        curlop_socklen_t len = sizeof(clientAddr);
                        auto newFd = ::accept(serverFd_,
                            reinterpret_cast<struct sockaddr*>(&clientAddr), &len);
                        if (isValidSocket(newFd)) {
                            // Find empty slot
                            int slot = -1;
                            for (int s = 0; s < MAX_CLIENTS; ++s) {
                                if (clients[s].isEmpty()) {
                                    slot = s;
                                    break;
                                }
                            }
                            if (slot >= 0) {
                                setNonBlocking(newFd);
                                clients[slot] = ClientState{};
                                clients[slot].fd = newFd;
                                fprintf(stderr, "WS_MULTI: Client connected, slot=%d fd=%lld\n",
                                    slot, socketLogValue(newFd));
                                fflush(stderr);
                            } else {
                                // No room — reject connection
                                fprintf(stderr, "WS_MULTI: No free slots, rejecting fd=%lld\n",
                                        socketLogValue(newFd));
                                fflush(stderr);
                                closeSocket(newFd);
                            }
                        }
                    }
                    else {
                        // ── Read data from client ──
                        int slot = fdToSlot[i];
                        ClientState& client = clients[slot];
                        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                            // Client disconnected
                            fprintf(stderr, "WS_MULTI: Client disconnected, slot=%d fd=%lld\n",
                                slot, socketLogValue(client.fd));
                            fflush(stderr);
                            retireClient (client, "poll_error");
                            continue;
                        }
                        if (fds[i].revents & POLLIN) {
                            uint8_t buf[65536];
                            ssize_t n = socketRecv(client.fd, buf, sizeof(buf));

                            if (n <= 0) {
                                fprintf(stderr, "WS_MULTI: Client disconnected, slot=%d fd=%lld\n",
                                    slot, socketLogValue(client.fd));
                                fflush(stderr);
                                retireClient (client, "recv_closed");
                                continue;
                            }

                            if (!client.upgraded) {
                                client.httpBuf.append(
                                    reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
                                tryWebSocketUpgrade(client);
                            } else {
                                client.rxBuf.insert(client.rxBuf.end(), buf, buf + n);
                                processWebSocketFrames(client);
                            }
                        }
                        if (isValidSocket(client.fd) && client.upgraded
                            && (fds[i].revents & POLLOUT)
                            && flushClientWrites(client) == PendingWriteStatus::Failed) {
                            retireClient (client, "write_failure");
                        }
                    }
                }
            }

            // ── Drain outbound queue: broadcast to all upgraded clients ──
            OutboundMessage outMsg;
            while (outQueue_.tryPop(outMsg)) {
                traceRequestLifecycle (RequestLifecycleStage::OutboundDequeue,
                                       outMsg.lifecycleRequestId);
                broadcastBinary(clients, outMsg.frame.data(), outMsg.frame.size(),
                                 outMsg.lifecycleRequestId);
            }
        }

        // ── Cleanup all clients ──
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            retireClient (clients[i], "server_stop");
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // WebSocket handshake (RFC 6455)
    // ═══════════════════════════════════════════════════════════════
    void tryWebSocketUpgrade(ClientState& client) {
        // Look for the end of HTTP headers
        auto pos = client.httpBuf.find("\r\n\r\n");
        if (pos == std::string::npos) return;

        // Extract Sec-WebSocket-Key
        std::string key = extractHeader(client.httpBuf, "Sec-WebSocket-Key");
        if (key.empty()) {
            retireClient (client, "handshake_error");
            return;
        }

        // Compute accept hash: SHA1(key + magic GUID) -> base64
        std::string accept = computeWebSocketAccept(key);

        // Send 101 Switching Protocols
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n"
            "\r\n";

        socketSend(client.fd, response.c_str(), response.size());
        client.upgraded = true;
        client.httpBuf.clear();
    }

    std::string extractHeader(const std::string& request, const std::string& name) {
        std::string search = name + ": ";
        auto pos = request.find(search);
        if (pos == std::string::npos) {
            // Case-insensitive fallback
            std::string lower = request;
            std::string lowerName = search;
            for (auto& c : lower) c = static_cast<char>(tolower(c));
            for (auto& c : lowerName) c = static_cast<char>(tolower(c));
            pos = lower.find(lowerName);
            if (pos == std::string::npos) return "";
        }
        auto start = pos + search.size();
        auto end = request.find("\r\n", start);
        if (end == std::string::npos) return "";
        return request.substr(start, end - start);
    }

    std::string computeWebSocketAccept(const std::string& key) {
        static const std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string concat = key + guid;

        const auto hash = sha1(concat);
        return serializeBase64(hash.data(), hash.size());
    }

    static std::array<unsigned char, 20> sha1(const std::string& input)
    {
        auto rotl = [](uint32_t v, uint32_t bits) {
            return (v << bits) | (v >> (32u - bits));
        };

        std::vector<uint8_t> bytes(input.begin(), input.end());
        const uint64_t bitLen = static_cast<uint64_t>(bytes.size()) * 8u;
        bytes.push_back(0x80);
        while ((bytes.size() % 64u) != 56u)
            bytes.push_back(0);
        for (int i = 7; i >= 0; --i)
            bytes.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xffu));

        uint32_t h0 = 0x67452301u;
        uint32_t h1 = 0xefcdab89u;
        uint32_t h2 = 0x98badcfeu;
        uint32_t h3 = 0x10325476u;
        uint32_t h4 = 0xc3d2e1f0u;

        for (size_t chunk = 0; chunk < bytes.size(); chunk += 64)
        {
            uint32_t w[80] {};
            for (int i = 0; i < 16; ++i)
            {
                const size_t o = chunk + static_cast<size_t>(i * 4);
                w[i] = (static_cast<uint32_t>(bytes[o]) << 24)
                     | (static_cast<uint32_t>(bytes[o + 1]) << 16)
                     | (static_cast<uint32_t>(bytes[o + 2]) << 8)
                     |  static_cast<uint32_t>(bytes[o + 3]);
            }
            for (int i = 16; i < 80; ++i)
                w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f = 0;
                uint32_t k = 0;
                if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
                else if (i < 40) { f = b ^ c ^ d;            k = 0x6ed9eba1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
                else             { f = b ^ c ^ d;            k = 0xca62c1d6u; }

                const uint32_t temp = rotl(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rotl(b, 30);
                b = a;
                a = temp;
            }

            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }

        std::array<unsigned char, 20> out {};
        const uint32_t h[5] { h0, h1, h2, h3, h4 };
        for (int i = 0; i < 5; ++i)
        {
            out[static_cast<size_t>(i * 4)]     = static_cast<unsigned char>((h[i] >> 24) & 0xffu);
            out[static_cast<size_t>(i * 4 + 1)] = static_cast<unsigned char>((h[i] >> 16) & 0xffu);
            out[static_cast<size_t>(i * 4 + 2)] = static_cast<unsigned char>((h[i] >> 8) & 0xffu);
            out[static_cast<size_t>(i * 4 + 3)] = static_cast<unsigned char>(h[i] & 0xffu);
        }
        return out;
    }

    static std::string serializeBase64(const unsigned char* data, size_t len) {
        static const char table[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(4 * ((len + 2) / 3));

        for (size_t i = 0; i < len; i += 3) {
            uint32_t triplet = (static_cast<uint32_t>(data[i]) << 16);
            if (i + 1 < len) triplet |= (static_cast<uint32_t>(data[i + 1]) << 8);
            if (i + 2 < len) triplet |= static_cast<uint32_t>(data[i + 2]);

            result += table[(triplet >> 18) & 0x3F];
            result += table[(triplet >> 12) & 0x3F];
            result += (i + 1 < len) ? table[(triplet >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? table[triplet & 0x3F] : '=';
        }
        return result;
    }

    // ═══════════════════════════════════════════════════════════════
    // WebSocket frame parser (RFC 6455)
    // ═══════════════════════════════════════════════════════════════
    void processWebSocketFrames(ClientState& client) {
        while (client.rxBuf.size() >= 2) {
            const uint8_t* data = client.rxBuf.data();
            size_t bufSize = client.rxBuf.size();

            WebSocketFrameHeader frame;
            const auto status = parseClientWebSocketFrameHeader(data, bufSize, frame);
            if (status == WebSocketFrameParseStatus::NeedMoreData)
                return;
            if (status == WebSocketFrameParseStatus::ProtocolError) {
                retireClient (client, "protocol_error");
                return;
            }

            // Extract mask key
            const uint8_t* maskKey = data + frame.headerLen;
            const uint8_t* payload = data + frame.headerLen + frame.maskLen;

            // Unmask payload (in-place on a copy)
            std::vector<uint8_t> unmasked(payload, payload + frame.payloadLen);
            if (maskKey) {
                for (size_t i = 0; i < frame.payloadLen; ++i) {
                    unmasked[i] ^= maskKey[i % 4];
                }
            }

            // Handle frame
            if (frame.opcode == 0x02) {
                // Binary frame — our protocol
                handleBinaryMessage(unmasked.data(), unmasked.size());
            } else if (frame.opcode == 0x08) {
                // Close frame
                retireClient (client, "peer_close");
                return;
            } else if (frame.opcode == 0x09) {
                // Ping — send pong
                emitPong(client.fd, unmasked);
            }
            // 0x0A = pong — ignore

            // Consume frame from buffer
            client.rxBuf.erase(client.rxBuf.begin(),
                               client.rxBuf.begin() + static_cast<long>(frame.totalLen));
        }
    }

    void emitPong(curlop_socket_t fd, const std::vector<uint8_t>& payload) {
        // Pong frame: FIN + opcode 0x0A, unmasked (server to client)
        std::vector<uint8_t> frame;
        frame.push_back(0x8A); // FIN + pong

        if (payload.size() < 126) {
            frame.push_back(static_cast<uint8_t>(payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        }
        // > 65535 pong payload: practically never happens

        frame.insert(frame.end(), payload.begin(), payload.end());
        socketSend(fd, frame.data(), frame.size());
    }

    PendingWriteStatus flushClientWrites(ClientState& client) {
        const auto status = drainPendingFrame(client.txBuf, client.txOffset,
            [&client] (const uint8_t* data, size_t size) {
                const auto sent = socketSend(client.fd, data, size);
                if (sent > 0)
                    return NonBlockingWriteAttempt {
                        NonBlockingWriteState::Sent, static_cast<size_t>(sent)
                    };
                return NonBlockingWriteAttempt {
                    sent < 0 && lastSocketErrorIsWouldBlock()
                        ? NonBlockingWriteState::WouldBlock
                        : NonBlockingWriteState::Failed,
                    0
                };
            });
        if (status == PendingWriteStatus::Complete) {
            client.txBuf.clear();
            client.txOffset = 0;
            settlePendingRequestLifecycles (
                client.pendingLifecycle,
                [this] (const std::string& requestId)
                {
                    traceRequestLifecycle (
                        RequestLifecycleStage::OutboundWrite, requestId);
                });
        } else if (status == PendingWriteStatus::Pending) {
            for (auto& lifecycle : client.pendingLifecycle) {
                if (! lifecycle.backpressureRecorded) {
                    traceRequestLifecycle (RequestLifecycleStage::OutboundBackpressure,
                                           lifecycle.requestId);
                    lifecycle.backpressureRecorded = true;
                }
            }
        } else {
            settlePendingRequestLifecycles (
                client.pendingLifecycle,
                [this] (const std::string& requestId)
                {
                    traceRequestLifecycle (
                        RequestLifecycleStage::OutboundFailure,
                        requestId, "write_failure");
                });
        }
        return status;
    }

    // Broadcast raw payload as binary frames to ALL upgraded clients.
    // Called ONLY from the I/O thread (runLoop).
    void broadcastBinary(ClientState* clients, const uint8_t* data, size_t len,
                         const std::string& lifecycleRequestId = {}) {
        const auto frame = makeServerBinaryFrame(data, len);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (isValidSocket(clients[i].fd) && clients[i].upgraded) {
                auto& client = clients[i];
                if (client.txOffset > 0) {
                    client.txBuf.erase(client.txBuf.begin(),
                        client.txBuf.begin() + static_cast<long>(client.txOffset));
                    client.txOffset = 0;
                }
                client.txBuf.insert(client.txBuf.end(), frame.begin(), frame.end());
                if (! lifecycleRequestId.empty())
                    client.pendingLifecycle.push_back ({ lifecycleRequestId, false });
                if (flushClientWrites(client) == PendingWriteStatus::Failed)
                    retireClient (client, "write_failure");
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // Binary message handler — dispatches by magic header
    // ═══════════════════════════════════════════════════════════════
    void handleBinaryMessage(const uint8_t* data, size_t len) {
        if (len < 4) return;

        uint32_t magic;
        std::memcpy(&magic, data, 4);

        fprintf(stderr, "WS_MSG: magic=0x%08X len=%zu\n", magic, len);
        fflush(stderr);

        CDBG(WIRE_ORDER, "RX bin magic=0x%08X len=%zu", magic, len);

        if (! isSupportedInboundMagic(magic)) {
            fprintf(stderr, "WS_MSG: UNKNOWN magic! first bytes: %02X %02X %02X %02X\n",
                data[0], data[1], data[2], data[3]);
            fflush(stderr);
            return;
        }

        switch (magic) {
            case MAGIC_DCMD: parseDiagCommand(data + 4, len - 4);     break;
        }
    }

    // Tier 1 (0.C-1) Step 8: parseParamInitMessage deleted.

    // ── DCMD: diagnostic commands ───────────────────────────────
    // Format: subcommand(u8) [+ payload]
    // 0x10 = MCP_COMMAND (+ JSON string)
    // 0x30 = SET_DEBUG_FLAGS (lo uint64 LE + optional complete hi uint64 LE)
    void parseDiagCommand(const uint8_t* data, size_t len) {
        if (len < 1) return;

        // 0x30: SET_DEBUG_FLAGS — direct atomic write, no queuing needed.
        // Format: 0x30(u8) + lo(uint64 LE) [exactly + hi(uint64 LE)].
        if (data[0] == 0x30) {
            CurlopDebug::Mask mask;
            if (! parseDebugFlagsPayload(data, len, CurlopDebug::getMask(), mask)) {
                fprintf(stderr, "[DEBUG_FLAGS] rejected malformed SET_DEBUG_FLAGS len=%zu\n", len);
                fflush(stderr);
                return;
            }
            CurlopDebug::set(mask);
            fprintf(stderr, "[DEBUG_FLAGS] set lo=0x%016llX hi=0x%016llX\n",
                    (unsigned long long)mask.lo, (unsigned long long)mask.hi);
            fflush(stderr);
            // s427 / SPEC-011 PATH-A-LIVENESS — emit the new flag state as a
            // snapshot so `q diagnose --flag X` can answer "is X currently
            // enabled?" without a readback MCP tool (0.E-3 architecture).
            BootEmits::emitDebugFlagsState(mask);
            return;
        }

        DiagMessage msg;
        if (! parseQueuedDiagPayload(data, len, msg)) {
            fprintf(stderr, "WS_DCMD: rejected retired/unknown subcommand=0x%02X len=%zu\n",
                    data[0], len);
            fflush(stderr);
            return;
        }
        // Keep the callback lifetime lock while both processor callbacks run.
        // clearInboundCallbacks() takes this same lock during processor
        // teardown, so a DCMD handler can never retain and call a closure that
        // captured an already-destroyed processor.
        std::lock_guard<std::mutex> callbackLock(inboundCallbackMutex_);
        if (! applyQueuedDiagAuthority(msg, readProjectAuthorityAtIngress_)) {
            fprintf(stderr, "WS_DCMD: rejected MCP_COMMAND without project authority\n");
            fflush(stderr);
            return;
        }
        fprintf(stderr, "WS_DCMD: MCP_COMMAND json=%s\n", msg.commandJson.c_str());

        msg.lifecycleRequestId = readWebViewPerformanceLifecycleRequestId (msg.commandJson);
        traceRequestLifecycle (RequestLifecycleStage::Admission,
                               msg.lifecycleRequestId);
        diagQueue.push(std::move(msg));
        if (onInboundMessageQueued_) onInboundMessageQueued_();
        fflush(stderr);
    }

    // ═══════════════════════════════════════════════════════════════
    // Utility
    // ═══════════════════════════════════════════════════════════════
    static void setNonBlocking(curlop_socket_t fd) {
#if JUCE_WINDOWS
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }
};

} // namespace curlop
