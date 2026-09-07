#ifndef SEGMENTPUZZLER_WORKERTHREADS_H
#define SEGMENTPUZZLER_WORKERTHREADS_H

namespace segment_puzzler {

enum class ThreadBackend { OpenMP, ITK, VTK };

// Selects the backend environment setting or the physical core count.
int workerThreadCount(ThreadBackend backend = ThreadBackend::OpenMP);

} // namespace segment_puzzler

#endif
