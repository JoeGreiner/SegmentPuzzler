#include "StartupVolumes.h"

#include <QMap>
#include <QRegularExpression>
#include <cmath>
#include <stdexcept>

std::vector<StartupVolume> parseStartupVolumes(const QStringList &arguments) {
    using Kind = StartupVolumeKind;
    const QMap<QString, Kind> kinds{
        {"--segments", Kind::Supervoxels}, {"--image", Kind::Image},
        {"--segmentation", Kind::Segmentation}, {"--boundary", Kind::Boundary},
        {"--refinement", Kind::Refinement}
    };
    QMap<Kind, std::vector<StartupVolume>> groups;
    StartupVolume *current = nullptr;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &arg = arguments[i];
        const auto kind = kinds.constFind(arg);
        if (kind != kinds.cend()) {
            if (i + 1 == arguments.size() || arguments[i + 1].startsWith("--"))
                throw std::invalid_argument((arg + " requires a file path").toStdString());
            auto &volumes = groups[kind.value()];
            if (kind.value() != Kind::Image && kind.value() != Kind::Segmentation)
                volumes.clear();
            volumes.push_back({kind.value(), arguments[++i], {}, {}});
            current = &volumes.back();
            if (i + 1 < arguments.size() && !arguments[i + 1].startsWith("--"))
                current->name = arguments[++i];
            continue;
        }
        if (!current)
            throw std::invalid_argument(("Expected a volume declaration before " + arg).toStdString());

        const auto error = [&](const QString &message) {
            return std::invalid_argument((current->path + ": " + arg + " " + message).toStdString());
        };
        const bool intensityLayer = current->kind == Kind::Image || current->kind == Kind::Boundary;

        auto nextValue = [&]() {
            if (i + 1 >= arguments.size() || arguments[i + 1].startsWith("--"))
                throw error("is missing a value");
            return arguments[++i];
        };
        auto integer = [&](int lower, int upper) {
            bool ok = false;
            const int value = nextValue().toInt(&ok, 10);
            if (!ok || value < lower || value > upper)
                throw error(QString("expects an integer from %1 to %2").arg(lower).arg(upper));
            return value;
        };
        auto requireUnset = [&](const auto &setting) {
            if (setting.has_value()) throw error("duplicates a layer setting");
        };
        auto &display = current->display;
        if (arg == "--visible" || arg == "--hidden") {
            requireUnset(display.visible);
            display.visible = arg == "--visible";
        } else if (arg == "--opacity") {
            requireUnset(display.opacityPercent);
            display.opacityPercent = integer(0, 100);
        } else if (arg == "--view") {
            if (intensityLayer) throw error("requires a label layer");
            requireUnset(display.view);
            const QString mode = nextValue();
            if (mode == "filled") display.view = itkSignalBase::LabelRenderMode::Filled;
            else if (mode == "outline") display.view = itkSignalBase::LabelRenderMode::Boundaries;
            else throw error("expects outline or filled");
        } else if (arg == "--outline-width") {
            if (intensityLayer) throw error("requires a label layer");
            requireUnset(display.outlineWidth);
            display.outlineWidth = integer(1, itkSignalBase::MaximumLabelBoundaryWidth);
        } else if (arg == "--color") {
            if (!intensityLayer) throw error("requires an image or boundary layer");
            requireUnset(display.color);
            if (arguments.value(i + 1).startsWith('#')) {
                const QString hex = nextValue();
                static const QRegularExpression rgbHex(QStringLiteral("\\A#[0-9a-fA-F]{6}\\z"));
                if (!rgbHex.match(hex).hasMatch()) throw error("expects '#RRGGBB' or R G B");
                display.color = QColor(hex);
            } else {
                const int red = integer(0, 255);
                const int green = integer(0, 255);
                const int blue = integer(0, 255);
                display.color = QColor(red, green, blue);
            }
        } else if (arg == "--blend") {
            if (!intensityLayer) throw error("requires an image or boundary layer");
            requireUnset(display.blend);
            const QString mode = nextValue();
            if (mode == "additive") display.blend = itkSignalBase::BlendMode::Additive;
            else if (mode == "over") display.blend = itkSignalBase::BlendMode::SourceOver;
            else throw error("expects additive or over");
        } else if (arg == "--range") {
            if (!intensityLayer) throw error("requires an image or boundary layer");
            requireUnset(display.range);
            bool lowerOk = false, upperOk = false;
            const double lower = nextValue().toDouble(&lowerOk);
            const double upper = nextValue().toDouble(&upperOk);
            if (!lowerOk || !upperOk || !std::isfinite(lower) || !std::isfinite(upper) || lower > upper)
                throw error("expects two finite intensities with MIN <= MAX");
            display.range = IntensityRange{lower, upper};
        } else {
            const QString message = arg.startsWith("--") ? "Unknown option: " : "Unexpected argument: ";
            throw std::invalid_argument((message + arg).toStdString());
        }
    }
    std::vector<StartupVolume> ordered;
    for (Kind kind : {Kind::Supervoxels, Kind::Image, Kind::Segmentation, Kind::Boundary, Kind::Refinement}) {
        const auto &group = groups[kind];
        ordered.insert(ordered.end(), group.begin(), group.end());
    }
    return ordered;
}
