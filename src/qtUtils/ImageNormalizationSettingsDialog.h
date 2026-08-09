#ifndef SEGMENTPUZZLER_IMAGENORMALIZATIONSETTINGSDIALOG_H
#define SEGMENTPUZZLER_IMAGENORMALIZATIONSETTINGSDIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class itkSignalBase;

namespace segment_puzzler::image_normalization {

enum class Strategy {
    DataRange = 0,
    Quantile = 1
};

struct Settings {
    Strategy strategy = Strategy::Quantile;
    double lowerQuantile = 0.001;
    double upperQuantile = 0.999;
    bool ignoreZero = true;
};

Settings defaultSettings();
Settings loadSettings();
void saveSettings(const Settings &settings);
bool computeRange(itkSignalBase *signal, const Settings &settings,
                  double &lower, double &upper);
bool applyToSignal(itkSignalBase *signal);
void configureContinuousDisplay(itkSignalBase *signal);
void configureLoadedImageDisplay(itkSignalBase *signal);

} // namespace segment_puzzler::image_normalization

class ImageNormalizationSettingsDialog : public QDialog {
public:
    explicit ImageNormalizationSettingsDialog(QWidget *parent = nullptr);

private:
    void accept() override;
    void loadIntoWidgets(const segment_puzzler::image_normalization::Settings &settings);
    segment_puzzler::image_normalization::Settings collectSettings() const;
    void updateQuantileControls();

    QComboBox *strategyComboBox = nullptr;
    QDoubleSpinBox *lowerQuantileSpinBox = nullptr;
    QDoubleSpinBox *upperQuantileSpinBox = nullptr;
    QCheckBox *ignoreZeroCheckBox = nullptr;
};

#endif // SEGMENTPUZZLER_IMAGENORMALIZATIONSETTINGSDIALOG_H
