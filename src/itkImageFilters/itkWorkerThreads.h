#ifndef SEGMENTPUZZLER_ITKWORKERTHREADS_H
#define SEGMENTPUZZLER_ITKWORKERTHREADS_H

#include "src/utils/WorkerThreads.h"
#include <itkMultiThreaderBase.h>
#include <itkProcessObject.h>

namespace segment_puzzler {

inline void setItkWorkerThreadCount(itk::ProcessObject &filter, int requested = 0) {
    const int count = workerThreadCount(requested, ThreadBackend::ITK);
    filter.GetMultiThreader()->SetMaximumNumberOfThreads(count);
    filter.SetNumberOfWorkUnits(count);
}

} // namespace segment_puzzler

#endif
