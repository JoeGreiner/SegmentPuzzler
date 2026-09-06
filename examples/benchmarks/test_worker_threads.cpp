#include "src/utils/WorkerThreads.h"
#include "src/utils/DistanceMapFH3D.h"
#include "src/utils/WatershedRagAgglomeration.h"
#include "src/itkImageFilters/itkWorkerThreads.h"
#include <itkBinaryThresholdImageFilter.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef USE_OMP
#include <omp.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class EnvironmentVariable {
public:
    explicit EnvironmentVariable(const char *name) : name_(name) {
        if (const char *value = std::getenv(name)) {
            previous_ = value;
        }
        set(nullptr);
    }
    ~EnvironmentVariable() { set(previous_ ? previous_->c_str() : nullptr); }
    void set(const char *value) const {
#ifdef _WIN32
        _putenv_s(name_, value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(name_, value, 1);
        } else {
            unsetenv(name_);
        }
#endif
    }
private:
    const char *name_;
    std::optional<std::string> previous_;
};

void testSelection() {
    using namespace segment_puzzler;
    EnvironmentVariable omp("OMP_NUM_THREADS");
    EnvironmentVariable itk("ITK_GLOBAL_DEFAULT_NUMBER_OF_THREADS");
    EnvironmentVariable vtk("VTK_SMP_MAX_THREADS");
    const int automatic = workerThreadCount(0, ThreadBackend::ITK);
    require(automatic >= 1, "Automatic selection must be positive.");
    require(workerThreadCount(0, ThreadBackend::VTK) == automatic, "Backend hardware defaults differ.");
    itk.set("3");
    vtk.set("5");
    omp.set("7, 2");
    require(workerThreadCount(0, ThreadBackend::ITK) == 3, "ITK environment ignored.");
    require(workerThreadCount(0, ThreadBackend::VTK) == 5, "VTK environment ignored.");
    require(workerThreadCount(11, ThreadBackend::ITK) == 11, "Explicit request did not take precedence.");
#ifdef USE_OMP
    require(workerThreadCount() == 7, "OpenMP list must select the first value.");
    require(workerThreadCount(11) == 11, "Explicit OpenMP request did not take precedence.");
#else
    require(workerThreadCount() == 1 && workerThreadCount(11) == 1, "OpenMP-disabled work must be serial.");
#endif
    for (const char *invalid : {"", " ", "0", "-2", "2147483648", "2x", "2,3"}) {
        itk.set(invalid);
        require(workerThreadCount(0, ThreadBackend::ITK) == automatic, "Invalid environment did not fall back.");
    }
    itk.set(" \t4 \n");
    require(workerThreadCount(0, ThreadBackend::ITK) == 4, "Whitespace around count rejected.");
#ifdef USE_OMP
    for (const char *invalid : {"", "0", "-1", "2147483648", "2x", "2,", ",2", "2,0", "2,x"}) {
        omp.set(invalid);
        require(workerThreadCount() == automatic, "Invalid OpenMP list did not fall back.");
    }
#endif
    for (auto backend : {ThreadBackend::OpenMP, ThreadBackend::ITK, ThreadBackend::VTK}) {
        bool rejected = false;
        try { workerThreadCount(-1, backend); }
        catch (const std::invalid_argument &) { rejected = true; }
        require(rejected, "Negative thread request accepted.");
    }
    std::cout << "Physical-core default: " << automatic << '\n';
}

void testRuntimeIsolation() {
    using namespace segment_puzzler;
    EnvironmentVariable omp("OMP_NUM_THREADS");
    EnvironmentVariable vtk("VTK_SMP_MAX_THREADS");
#ifdef USE_OMP
    const int previous = omp_get_max_threads();
    omp_set_num_threads(1);
    require(workerThreadCount() == workerThreadCount(0, ThreadBackend::VTK),
            "Automatic selection depends on an earlier OpenMP setter.");
#endif
    const std::array<int, 3> dimensions{17, 19, 23};
    const std::array<double, 3> spacing{0.7, 1.3, 2.1};
    std::vector<unsigned char> mask(17 * 19 * 23);
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = (index * 37 % 101) > 10;
    }
    const auto serial = distance_map_benchmark::runBoundaryAwareSquaredEdt(mask, dimensions, spacing, 1);
    const auto parallel = distance_map_benchmark::runBoundaryAwareSquaredEdt(mask, dimensions, spacing, 4);
    require(serial.distances == parallel.distances, "FH output changed with thread count.");
#ifdef USE_OMP
    require(omp_get_max_threads() == 1, "FH changed the caller's OpenMP default.");
#endif
    auto labels = dataType::SegmentsImageType::New();
    dataType::SegmentsImageType::SizeType size;
    size.Fill(3);
    labels->SetRegions(size);
    labels->Allocate();
    labels->FillBuffer(1);
    labels->GetBufferPointer()[0] = 2;
    auto boundary = BoundaryFloatImageType::New();
    boundary->SetRegions(size);
    boundary->Allocate();
    boundary->FillBuffer(0);
    WatershedRagAgglomerationOptions options;
    options.threadCount = 4;
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;
    const auto result = runWatershedRagAgglomeration(labels, boundary, nullptr, options);
    require(result.stats.outputClusterCount == 1, "RAG fixture did not merge.");
#ifdef USE_OMP
    require(omp_get_max_threads() == 1, "RAG changed the caller's OpenMP default.");
    omp_set_num_threads(previous);
#endif
}

void testFilterIsolation() {
    using namespace segment_puzzler;
    const auto previous = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads();
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(1);
    using Image = dataType::SegmentsImageType;
    auto filter = itk::BinaryThresholdImageFilter<Image, Image>::New();
    setItkWorkerThreadCount(*filter, 4);
    require(filter->GetNumberOfWorkUnits() == 4, "ITK filter work units were not set.");
    require(filter->GetMultiThreader()->GetMaximumNumberOfThreads() == 4,
            "ITK filter retained the startup thread limit.");
    require(itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads() == 1,
            "Configuring an ITK filter changed the global default.");
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(previous);
}

} // namespace

int main() {
    try {
        testSelection();
        testRuntimeIsolation();
        testFilterIsolation();
        std::cout << "Worker thread tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
