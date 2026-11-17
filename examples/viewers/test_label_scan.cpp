#include "src/utils/LabelScan.h"
#include <iostream>
#include <limits>

int main() {
    using Label = std::uint32_t;
    const auto maximum = std::numeric_limits<Label>::max();
    std::vector<Label> runs(10003, 7);
    std::fill(runs.begin() + 123, runs.begin() + 7654, 0);
    runs.back() = maximum;
    std::vector<Label> alternating(10003);
    for (std::size_t i = 0; i < alternating.size(); ++i) alternating[i] = i % 5;
    const std::vector<std::vector<Label>> cases{{}, {0}, {7}, {maximum},
        {0, 7, 1000000, maximum, 7, 0}, runs, alternating};
    for (const auto &pixels : cases) {
        for (const Label background : {Label{0}, Label{7}, maximum}) {
            for (const bool collect : {false, true}) {
                std::unordered_set<Label> expected{42}, actual{42};
                std::uint64_t expectedNext = 0;
                for (const auto label : pixels) {
                    if (collect) expected.insert(label);
                    if (label != background) expectedNext = std::max(expectedNext, std::uint64_t(label) + 1);
                }
                const auto next = segment_puzzler::scanLabels(pixels.data(), pixels.size(), background,
                                                             collect ? &actual : nullptr);
                if (actual != expected || next != expectedNext) {
                    std::cerr << "Parallel label scan differs from sequential reference\n";
                    return 1;
                }
            }
        }
    }
    std::cout << "Label scan tests passed\n";
}
