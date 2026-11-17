#pragma once

#include "src/file_definitions/dataTypes.h"
#include <QByteArray>
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace volume_io {
using Coordinates = std::array<std::uint64_t, 3>;
using Label = dataType::SegmentIdType;

struct CropRegion {
    Coordinates start{{0, 0, 0}};
    Coordinates size{{1, 1, 1}};
};

struct ImageLoadOptions {
    std::optional<CropRegion> crop;
    bool allowFullRead = false;
    bool labelImage = false;
    bool backgroundIsHighest = false;
};

enum class LabelPolicy { SameSegmentation, Independent };

struct TargetBinding {
    QString path;
    Coordinates start{};
    LabelPolicy policy = LabelPolicy::Independent;
    Label background = 0;
    QByteArray fingerprint;
    std::unordered_map<Label, Label> labels;
};

struct VolumeContext {
    QString sourcePath;
    Coordinates originalSize{};
    std::array<double, 3> spacing{{1, 1, 1}};
    std::array<double, 3> origin{};
    std::array<double, 9> direction{{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    std::optional<CropRegion> crop;
    QByteArray sourceFingerprint;
    std::optional<Label> background;
    std::unordered_set<Label> originalLabels;
    // uint64 holds UINT32_MAX + 1 when the label range is exhausted.
    std::uint64_t nextLabel = 1;
    std::vector<TargetBinding> targets;
};

// The layer owns this context; recovery files persist it in image metadata.
void initializeLabels(VolumeContext &context, const Label *pixels, std::size_t count,
                      bool rememberOriginalLabels);
std::uint64_t nextAvailableLabel(std::uint64_t next, const std::unordered_set<Label> &reserved);
Label firstLabelForReservation(const VolumeContext &context, std::size_t count);
Label reserveLabels(VolumeContext &context, std::size_t count);
void useLabel(VolumeContext &context, Label label);
std::shared_ptr<VolumeContext> derivedContext(const std::shared_ptr<VolumeContext> &source);

} // namespace volume_io
