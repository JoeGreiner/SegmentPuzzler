#include "examples/benchmarks/BenchmarkPerfettoTracing.h"

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#if SEGMENT_PUZZLER_HAS_PERFETTO
PERFETTO_TRACK_EVENT_STATIC_STORAGE();
#endif

namespace benchmark_tracing {
namespace {

int openTraceFile(const std::string &outputPath) {
#if defined(_WIN32)
    return _open(outputPath.c_str(), _O_CREAT | _O_TRUNC | _O_BINARY | _O_WRONLY, _S_IREAD | _S_IWRITE);
#else
    return ::open(outputPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
#endif
}

void closeTraceFile(int fd) {
    if (fd < 0) {
        return;
    }
#if defined(_WIN32)
    _close(fd);
#else
    ::close(fd);
#endif
}

std::once_flag perfettoInitFlag;

} // namespace

PerfettoTraceSession::PerfettoTraceSession(PerfettoTraceSession &&other) noexcept
#if SEGMENT_PUZZLER_HAS_PERFETTO
    : session_(std::move(other.session_))
#endif
{
    traceFd_ = other.traceFd_;
    outputPath_ = std::move(other.outputPath_);
    other.traceFd_ = -1;
}

PerfettoTraceSession &PerfettoTraceSession::operator=(PerfettoTraceSession &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    stop();
#if SEGMENT_PUZZLER_HAS_PERFETTO
    session_ = std::move(other.session_);
#endif
    traceFd_ = other.traceFd_;
    outputPath_ = std::move(other.outputPath_);
    other.traceFd_ = -1;
    return *this;
}

PerfettoTraceSession::~PerfettoTraceSession() {
    stop();
}

void PerfettoTraceSession::initializeOnce() {
#if SEGMENT_PUZZLER_HAS_PERFETTO
    std::call_once(perfettoInitFlag, [] {
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kInProcessBackend;
        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();
    });
#endif
}

PerfettoTraceSession PerfettoTraceSession::start(const std::string &outputPath, std::size_t bufferKb) {
#if !SEGMENT_PUZZLER_HAS_PERFETTO
    (void) outputPath;
    (void) bufferKb;
    throw std::runtime_error(
        "Perfetto SDK not configured. Run scripts/setup_perfetto_sdk.sh or set "
        "SEGMENT_PUZZLER_PERFETTO_SDK_DIR to a directory containing perfetto.h and perfetto.cc.");
#else
    initializeOnce();

    std::filesystem::path tracePath(outputPath);
    if (tracePath.has_parent_path()) {
        std::filesystem::create_directories(tracePath.parent_path());
    }

    PerfettoTraceSession traceSession;
    traceSession.outputPath_ = outputPath;
    traceSession.traceFd_ = openTraceFile(outputPath);
    if (traceSession.traceFd_ < 0) {
        throw std::runtime_error("Failed to open Perfetto trace output: " + outputPath);
    }

    traceSession.session_ = perfetto::Tracing::NewTrace();
    if (!traceSession.session_) {
        closeTraceFile(traceSession.traceFd_);
        traceSession.traceFd_ = -1;
        throw std::runtime_error("Failed to create Perfetto tracing session.");
    }

    perfetto::TraceConfig traceConfig;
    auto *buffer = traceConfig.add_buffers();
    buffer->set_size_kb(static_cast<uint32_t>(bufferKb));
    auto *dataSource = traceConfig.add_data_sources()->mutable_config();
    dataSource->set_name("track_event");
    traceConfig.set_write_into_file(true);
    traceSession.session_->Setup(traceConfig, traceSession.traceFd_);
    traceSession.session_->StartBlocking();
    return traceSession;
#endif
}

void PerfettoTraceSession::stop() {
#if !SEGMENT_PUZZLER_HAS_PERFETTO
    closeTraceFile(traceFd_);
    traceFd_ = -1;
#else
    if (!session_) {
        return;
    }

    session_->FlushBlocking(5000);
    session_->StopBlocking();
    session_.reset();
    closeTraceFile(traceFd_);
    traceFd_ = -1;
#endif
}

} // namespace benchmark_tracing
