#include "ImageNormalizationSettingsDialog.h"

#include "src/utils/AppLogger.h"
#include "src/viewers/itkSignalBase.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace segment_puzzler::image_normalization {
namespace {

constexpr char kSettingsGroup[] = "ImageNormalization";
constexpr char kStrategyKey[] = "strategy";
constexpr char kLowerQuantileKey[] = "lowerQuantile";
constexpr char kUpperQuantileKey[] = "upperQuantile";
constexpr char kIgnoreZeroKey[] = "ignoreZero";

Settings validatedSettings(Settings settings) {
    const Settings defaults = defaultSettings();
    if (settings.strategy != Strategy::DataRange
        && settings.strategy != Strategy::Quantile) {
        settings.strategy = defaults.strategy;
    }
    settings.lowerQuantile = std::clamp(settings.lowerQuantile, 0.0, 1.0);
    settings.upperQuantile = std::clamp(settings.upperQuantile, 0.0, 1.0);
    if (settings.lowerQuantile >= settings.upperQuantile) {
        settings.lowerQuantile = defaults.lowerQuantile;
        settings.upperQuantile = defaults.upperQuantile;
    }
    return settings;
}

QString strategyName(Strategy strategy) {
    return strategy == Strategy::Quantile
               ? QStringLiteral("quantile")
               : QStringLiteral("data_range");
}

} // namespace

Settings defaultSettings() {
    return {};
}

Settings loadSettings() {
    const Settings defaults = defaultSettings();
    QSettings storedSettings;
    storedSettings.beginGroup(QString::fromLatin1(kSettingsGroup));

    Settings settings;
    settings.strategy = static_cast<Strategy>(
        storedSettings.value(QString::fromLatin1(kStrategyKey),
                             static_cast<int>(defaults.strategy)).toInt());
    settings.lowerQuantile =
        storedSettings.value(QString::fromLatin1(kLowerQuantileKey),
                             defaults.lowerQuantile).toDouble();
    settings.upperQuantile =
        storedSettings.value(QString::fromLatin1(kUpperQuantileKey),
                             defaults.upperQuantile).toDouble();
    settings.ignoreZero =
        storedSettings.value(QString::fromLatin1(kIgnoreZeroKey),
                             defaults.ignoreZero).toBool();
    storedSettings.endGroup();
    return validatedSettings(settings);
}

void saveSettings(const Settings &settings) {
    const Settings validated = validatedSettings(settings);
    QSettings storedSettings;
    storedSettings.beginGroup(QString::fromLatin1(kSettingsGroup));
    storedSettings.setValue(QString::fromLatin1(kStrategyKey),
                            static_cast<int>(validated.strategy));
    storedSettings.setValue(QString::fromLatin1(kLowerQuantileKey),
                            validated.lowerQuantile);
    storedSettings.setValue(QString::fromLatin1(kUpperQuantileKey),
                            validated.upperQuantile);
    storedSettings.setValue(QString::fromLatin1(kIgnoreZeroKey),
                            validated.ignoreZero);
    storedSettings.endGroup();
    storedSettings.sync();
}

bool computeRange(itkSignalBase *signal, const Settings &settings,
                  double &lower, double &upper) {
    if (signal == nullptr || !signal->supportsNormControl()) {
        return false;
    }

    const Settings validated = validatedSettings(settings);
    if (validated.strategy == Strategy::DataRange) {
        lower = signal->getMinimumValueAsDouble();
        upper = signal->getMaximumValueAsDouble();
        return std::isfinite(lower) && std::isfinite(upper) && lower <= upper;
    }

    return signal->computeQuantileContrastRange(
        validated.lowerQuantile,
        validated.upperQuantile,
        validated.ignoreZero,
        lower,
        upper);
}

bool applyToSignal(itkSignalBase *signal) {
    const Settings settings = loadSettings();
    double lower = 0.0;
    double upper = 0.0;
    if (!computeRange(signal, settings, lower, upper)) {
        return false;
    }

    signal->setNorm(lower, upper);
    SP_LOG_INFO(
        "viewer.render",
        QStringLiteral("operation=auto_contrast strategy=%1 lower_quantile=%2 "
                       "upper_quantile=%3 ignore_zero=%4 range=[%5,%6]")
            .arg(strategyName(settings.strategy))
            .arg(settings.lowerQuantile, 0, 'g', 6)
            .arg(settings.upperQuantile, 0, 'g', 6)
            .arg(settings.ignoreZero)
            .arg(lower, 0, 'g', 9)
            .arg(upper, 0, 'g', 9));
    return true;
}

void configureContinuousDisplay(itkSignalBase *signal) {
    if (signal == nullptr) {
        return;
    }
    signal->setBlendMode(itkSignalBase::BlendMode::Additive);
    signal->setAlpha(255);
    signal->setContinuousColorMode();
}

void configureLoadedImageDisplay(itkSignalBase *signal) {
    configureContinuousDisplay(signal);
    applyToSignal(signal);
}

} // namespace segment_puzzler::image_normalization

ImageNormalizationSettingsDialog::ImageNormalizationSettingsDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Image Normalization"));

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        tr("These settings are applied once when a continuous image is loaded "
           "and whenever Auto contrast is requested."),
        this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout();
    strategyComboBox = new QComboBox(this);
    strategyComboBox->addItem(
        tr("Quantile clipping"),
        static_cast<int>(segment_puzzler::image_normalization::Strategy::Quantile));
    strategyComboBox->addItem(
        tr("Full data range (min/max)"),
        static_cast<int>(segment_puzzler::image_normalization::Strategy::DataRange));

    const auto createQuantileSpinBox = [this]() {
        auto *spinBox = new QDoubleSpinBox(this);
        spinBox->setRange(0.0, 100.0);
        spinBox->setDecimals(3);
        spinBox->setSingleStep(0.1);
        spinBox->setSuffix(tr(" %"));
        return spinBox;
    };
    lowerQuantileSpinBox = createQuantileSpinBox();
    upperQuantileSpinBox = createQuantileSpinBox();
    ignoreZeroCheckBox = new QCheckBox(tr("Ignore zero-valued voxels"), this);

    form->addRow(tr("Strategy:"), strategyComboBox);
    form->addRow(tr("Lower quantile:"), lowerQuantileSpinBox);
    form->addRow(tr("Upper quantile:"), upperQuantileSpinBox);
    form->addRow(QString(), ignoreZeroCheckBox);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ImageNormalizationSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, [this]() {
                loadIntoWidgets(segment_puzzler::image_normalization::defaultSettings());
            });
    connect(strategyComboBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this]() { updateQuantileControls(); });
    layout->addWidget(buttons);

    loadIntoWidgets(segment_puzzler::image_normalization::loadSettings());
}

void ImageNormalizationSettingsDialog::accept() {
    const auto settings = collectSettings();
    if (settings.strategy == segment_puzzler::image_normalization::Strategy::Quantile
        && settings.lowerQuantile >= settings.upperQuantile) {
        QMessageBox::warning(
            this,
            tr("Image Normalization"),
            tr("The lower quantile must be smaller than the upper quantile."));
        return;
    }
    segment_puzzler::image_normalization::saveSettings(settings);
    QDialog::accept();
}

void ImageNormalizationSettingsDialog::loadIntoWidgets(
    const segment_puzzler::image_normalization::Settings &settings) {
    const int strategyIndex = strategyComboBox->findData(static_cast<int>(settings.strategy));
    strategyComboBox->setCurrentIndex(std::max(0, strategyIndex));
    lowerQuantileSpinBox->setValue(settings.lowerQuantile * 100.0);
    upperQuantileSpinBox->setValue(settings.upperQuantile * 100.0);
    ignoreZeroCheckBox->setChecked(settings.ignoreZero);
    updateQuantileControls();
}

segment_puzzler::image_normalization::Settings
ImageNormalizationSettingsDialog::collectSettings() const {
    segment_puzzler::image_normalization::Settings settings;
    settings.strategy = static_cast<segment_puzzler::image_normalization::Strategy>(
        strategyComboBox->currentData().toInt());
    settings.lowerQuantile = lowerQuantileSpinBox->value() / 100.0;
    settings.upperQuantile = upperQuantileSpinBox->value() / 100.0;
    settings.ignoreZero = ignoreZeroCheckBox->isChecked();
    return settings;
}

void ImageNormalizationSettingsDialog::updateQuantileControls() {
    const bool quantileEnabled =
        strategyComboBox->currentData().toInt()
        == static_cast<int>(segment_puzzler::image_normalization::Strategy::Quantile);
    lowerQuantileSpinBox->setEnabled(quantileEnabled);
    upperQuantileSpinBox->setEnabled(quantileEnabled);
    ignoreZeroCheckBox->setEnabled(quantileEnabled);
}
