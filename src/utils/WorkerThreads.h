#ifndef SEGMENTPUZZLER_WORKERTHREADS_H
#define SEGMENTPUZZLER_WORKERTHREADS_H

namespace segment_puzzler {

enum class ThreadBackend { OpenMP, ITK, VTK };

// Zero selects the backend environment setting or the physical core count.
// Negative requests are invalid. OpenMP work is serial in builds without OpenMP.
int workerThreadCount(int requested = 0, ThreadBackend backend = ThreadBackend::OpenMP);

} // namespace segment_puzzler

#endif
