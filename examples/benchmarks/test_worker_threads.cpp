#include "src/utils/WorkerThreads.h"
#include "src/utils/MinMaxValues.h"
#include "src/utils/DistanceMapFH3D.h"
#include "src/utils/WatershedRagAgglomeration.h"
#include "src/itkImageFilters/itkWorkerThreads.h"
#include <itkBinaryThresholdImageFilter.h>
#include <itkMinimumMaximumImageCalculator.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

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
    const int automatic = workerThreadCount(ThreadBackend::ITK);
    require(automatic >= 1, "Automatic selection must be positive.");
    require(workerThreadCount(ThreadBackend::VTK) == automatic, "Backend hardware defaults differ.");
    itk.set("3");
    vtk.set("5");
    omp.set("7, 2");
    require(workerThreadCount(ThreadBackend::ITK) == 3, "ITK environment ignored.");
    require(workerThreadCount(ThreadBackend::VTK) == 5, "VTK environment ignored.");
#ifdef USE_OMP
    require(workerThreadCount() == 7, "OpenMP list must select the first value.");
#else
    require(workerThreadCount() == 1, "OpenMP-disabled work must be serial.");
#endif
    for (const char *invalid : {"", " ", "0", "-2", "2147483648", "2x", "2,3"}) {
        itk.set(invalid);
        require(workerThreadCount(ThreadBackend::ITK) == automatic, "Invalid environment did not fall back.");
    }
    itk.set(" \t4 \n");
    require(workerThreadCount(ThreadBackend::ITK) == 4, "Whitespace around count rejected.");
#ifdef USE_OMP
    for (const char *invalid : {"", "0", "-1", "2147483648", "2x", "2,", ",2", "2,0", "2,x"}) {
        omp.set(invalid);
        require(workerThreadCount() == automatic, "Invalid OpenMP list did not fall back.");
    }
#endif
}

void testRuntimeIsolation(bool printResult) {
    using namespace segment_puzzler;
#ifdef USE_OMP
    const int selected = workerThreadCount();
    const int previous = omp_get_max_threads();
    omp_set_num_threads(1);
    require(workerThreadCount() == selected,
            "Automatic selection depends on an earlier OpenMP setter.");
#endif
    const std::array<int, 3> dimensions{17, 19, 23};
    const std::array<double, 3> spacing{0.7, 1.3, 2.1};
    std::vector<unsigned char> mask(17 * 19 * 23);
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = (index * 37 % 101) > 10;
    }
    require(minMaxValues(mask.data(), mask.size()) == std::make_pair<unsigned char, unsigned char>(0, 1),
            "Min/max of the binary mask is incorrect.");
#ifdef USE_OMP
    require(omp_get_max_threads() == 1, "Min/max changed the caller's OpenMP default.");
#endif
    const auto distances = distance_map_benchmark::runBoundaryAwareSquaredEdt(mask, dimensions, spacing);
    if (printResult) {
        for (float distance : distances.distances) {
            std::cout << std::hexfloat << distance << ",";
        }
        std::cout << '\n';
    }
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
    options.boundaryEvidenceStrategy = BoundaryEvidenceStrategy::RawInterfaceMean;
    setAgglomerationLogSink([](const std::string &) {});
    const auto result = runWatershedRagAgglomeration(labels, boundary, nullptr, options);
    setAgglomerationLogSink({});
    require(result.stats.outputClusterCount == 1, "RAG fixture did not merge.");
#ifdef USE_OMP
    require(omp_get_max_threads() == 1, "RAG changed the caller's OpenMP default.");
    omp_set_num_threads(previous);
#endif
}

void testFilterIsolation() {
    using namespace segment_puzzler;
    const auto defaultThreadCount = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads();
    using Image = dataType::SegmentsImageType;
    auto filter = itk::BinaryThresholdImageFilter<Image, Image>::New();
    setItkWorkerThreadCount(*filter);
    require(filter->GetNumberOfWorkUnits() == workerThreadCount(ThreadBackend::ITK), "ITK filter work units were not set.");
    require(itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads() == defaultThreadCount,
            "Configuring an ITK filter changed the global default.");
}

template<typename T>
void checkMinMax(const std::vector<T> &values) {
    using Image = itk::Image<T, 1>;
    auto image = Image::New();
    typename Image::SizeType size{{values.size()}};
    image->SetRegions(size);
    image->Allocate();
    std::copy(values.begin(), values.end(), image->GetBufferPointer());
    auto reference = itk::MinimumMaximumImageCalculator<Image>::New();
    reference->SetImage(image);
    reference->Compute();
    const auto result = segment_puzzler::minMaxValues(values.data(), values.size());
    require(result.first == reference->GetMinimum() && result.second == reference->GetMaximum(),
            "Buffer min/max differs from ITK.");
    require(std::signbit(result.first) == std::signbit(reference->GetMinimum()) &&
            std::signbit(result.second) == std::signbit(reference->GetMaximum()),
            "Buffer min/max changed the sign of zero.");
}

template<typename T>
void testMinMax() {
    checkMinMax<T>({});
    checkMinMax<T>({0});
    checkMinMax<T>({3, 3, 3, 3, 3});
    checkMinMax<T>({7, 0, 2, std::numeric_limits<T>::max(), 1, std::numeric_limits<T>::lowest(), 4, 2, 6});
    if constexpr (std::is_floating_point_v<T>) {
        const T nan = std::numeric_limits<T>::quiet_NaN();
        const T inf = std::numeric_limits<T>::infinity();
        checkMinMax<T>({-8, -2, -4, -9, -3});
        checkMinMax<T>({nan, -3, nan, -8, 2, nan, 7, nan, -1});
        checkMinMax<T>({nan, nan, nan});
        checkMinMax<T>({inf, inf});
        checkMinMax<T>({-inf, -inf});
        checkMinMax<T>({nan, inf, -inf, 0});
        checkMinMax<T>({-T(0), T(0)});
        checkMinMax<T>({T(0), -T(0)});
    }
}

} // namespace

int main(int argc, char **argv) {
    const bool printResult = argc == 2 && std::string(argv[1]) == "--fingerprint";
    try {
        testSelection();
        testRuntimeIsolation(printResult);
        testFilterIsolation();
        testMinMax<unsigned int>();
        testMinMax<unsigned long long>();
        testMinMax<int>();
        testMinMax<float>();
        testMinMax<double>();
        if (!printResult) std::cout << "Worker thread tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
