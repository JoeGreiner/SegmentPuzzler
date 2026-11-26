#pragma once

#include "itkSignalBase.h"
#include <optional>

struct IntensityRange {
    double lower;
    double upper;
};

struct LayerDisplayOptions {
    std::optional<bool> visible;
    std::optional<int> opacityPercent;
    std::optional<itkSignalBase::LabelRenderMode> view;
    std::optional<QColor> color;
    std::optional<itkSignalBase::BlendMode> blend;
    std::optional<IntensityRange> range;
    std::optional<int> outlineWidth;
};
