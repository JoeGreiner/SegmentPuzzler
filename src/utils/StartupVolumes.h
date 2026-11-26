#pragma once

#include "src/viewers/LayerDisplayOptions.h"
#include <QStringList>
#include <vector>

enum class StartupVolumeKind { Supervoxels, Image, Segmentation, Boundary, Refinement };

struct StartupVolume {
    StartupVolumeKind kind;
    QString path;
    QString name;
    LayerDisplayOptions display;
};

// Returns volumes in dependency order, preserving input order within each kind.
std::vector<StartupVolume> parseStartupVolumes(const QStringList &arguments);
