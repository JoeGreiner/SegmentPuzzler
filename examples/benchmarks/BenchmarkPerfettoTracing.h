#pragma once

#include <cstddef>
#include <memory>
#include <string>

#ifndef SEGMENT_PUZZLER_HAS_PERFETTO
#define SEGMENT_PUZZLER_HAS_PERFETTO 0
#endif

#if SEGMENT_PUZZLER_HAS_PERFETTO
#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("benchmark"),
    perfetto::Category("benchmark.case"),
    perfetto::Category("distance_map"),
    perfetto::Category("seeds"),
    perfetto::Category("watershed"),
    perfetto::Category("watershed.queue"),
    perfetto::Category("watershed.parallel"),
    perfetto::Category("watershed.debug"),
    perfetto::Category("watershed.slow"),
    perfetto::Category("agglomeration"),
    perfetto::Category("agglomeration.batch_select"),
    perfetto::Category("agglomeration.batch_reduce"),
    perfetto::Category("agglomeration.batch_apply"),
    perfetto::Category("agglomeration.rag_build"),
    perfetto::Category("agglomeration.projection"),
    perfetto::Category("compare"));
#else
#define TRACE_EVENT(...) ((void) 0)
#define TRACE_EVENT_BEGIN(...) ((void) 0)
#define TRACE_EVENT_END(...) ((void) 0)
#define TRACE_EVENT_INSTANT(...) ((void) 0)
#define TRACE_COUNTER(...) ((void) 0)
#define TRACE_EVENT_CATEGORY_ENABLED(...) false
#endif

namespace benchmark_tracing {

class PerfettoTraceSession {
public:
    PerfettoTraceSession() = default;
    PerfettoTraceSession(const PerfettoTraceSession &) = delete;
    PerfettoTraceSession &operator=(const PerfettoTraceSession &) = delete;
    PerfettoTraceSession(PerfettoTraceSession &&other) noexcept;
    PerfettoTraceSession &operator=(PerfettoTraceSession &&other) noexcept;
    ~PerfettoTraceSession();

    static void initializeOnce();
    static PerfettoTraceSession start(const std::string &outputPath, std::size_t bufferKb);

    bool enabled() const {
#if SEGMENT_PUZZLER_HAS_PERFETTO
        return session_ != nullptr;
#else
        return false;
#endif
    }
    const std::string &outputPath() const { return outputPath_; }

private:
    void stop();

#if SEGMENT_PUZZLER_HAS_PERFETTO
    std::unique_ptr<perfetto::TracingSession> session_;
#endif
    int traceFd_ = -1;
    std::string outputPath_;
};

} // namespace benchmark_tracing
