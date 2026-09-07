#ifndef SEGMENTPUZZLER_NONOVERLAPPINGTRANSFERSETTINGS_H
#define SEGMENTPUZZLER_NONOVERLAPPINGTRANSFERSETTINGS_H

#include <QSettings>
#include <cmath>

namespace segment_puzzler::non_overlapping_transfer {

struct Settings {
    double maximumOverlapFraction = 0.0;
    bool preserveExistingSegmentation = true;
};

inline constexpr char kMaximumOverlapKey[] = "NonOverlappingTransfer/maximumOverlapFraction";
inline constexpr char kPreserveSegmentationKey[] = "NonOverlappingTransfer/preserveExistingSegmentation";

inline double validatedOverlapFraction(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0 ? value : 0.0;
}

inline Settings loadSettings() {
    QSettings stored;
    Settings settings;
    settings.maximumOverlapFraction = validatedOverlapFraction(
        stored.value(QLatin1String(kMaximumOverlapKey), settings.maximumOverlapFraction).toDouble());
    const QString preserveValue = stored.value(
        QLatin1String(kPreserveSegmentationKey), settings.preserveExistingSegmentation).toString().toLower();
    settings.preserveExistingSegmentation = preserveValue != QLatin1String("false") && preserveValue != QLatin1String("0");
    return settings;
}

inline void saveSettings(const Settings &settings) {
    QSettings stored;
    stored.setValue(QLatin1String(kMaximumOverlapKey), validatedOverlapFraction(settings.maximumOverlapFraction));
    stored.setValue(QLatin1String(kPreserveSegmentationKey), settings.preserveExistingSegmentation);
    stored.sync();
}

} // namespace segment_puzzler::non_overlapping_transfer

#endif
