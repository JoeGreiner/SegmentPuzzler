#include "LoggingSettingsDialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QUrl>
#include <QVBoxLayout>

using segment_puzzler::app_logging::AppLogger;
using segment_puzzler::app_logging::LogLevel;
using segment_puzzler::app_logging::LogSettings;

namespace {

void addLevelOption(QComboBox *comboBox, const QString &label, LogLevel level) {
    comboBox->addItem(label, static_cast<int>(level));
}

QString nearestExistingDirectoryPath(const QString &path) {
    if (path.isEmpty()) {
        return QString();
    }

    QString currentPath = QDir(path).absolutePath();
    while (!currentPath.isEmpty()) {
        const QDir currentDir(currentPath);
        if (currentDir.exists()) {
            return currentDir.absolutePath();
        }

        const QString parentPath = QFileInfo(currentPath).dir().absolutePath();
        if (parentPath == currentPath) {
            break;
        }
        currentPath = parentPath;
    }

    return QString();
}

} // namespace

LoggingSettingsDialog::LoggingSettingsDialog(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Logging"));
    setModal(true);
    resize(620, 0);

    auto *mainLayout = new QVBoxLayout(this);

    auto *generalGroup = new QGroupBox(QStringLiteral("General"), this);
    auto *generalLayout = new QFormLayout(generalGroup);
    generalLayout->setContentsMargins(12, 12, 12, 12);
    generalLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    minimumLevelComboBox_ = new QComboBox(generalGroup);
    addLevelOption(minimumLevelComboBox_, QStringLiteral("Trace"), LogLevel::Trace);
    addLevelOption(minimumLevelComboBox_, QStringLiteral("Debug"), LogLevel::Debug);
    addLevelOption(minimumLevelComboBox_, QStringLiteral("Info"), LogLevel::Info);
    addLevelOption(minimumLevelComboBox_, QStringLiteral("Warning"), LogLevel::Warning);
    addLevelOption(minimumLevelComboBox_, QStringLiteral("Error"), LogLevel::Error);
    configureMinimumLevelComboBox();
    generalLayout->addRow(QStringLiteral("Minimum Level"), minimumLevelComboBox_);

    consoleEnabledCheckBox_ = new QCheckBox(QStringLiteral("Write logs to the terminal"), generalGroup);
    generalLayout->addRow(QString(), consoleEnabledCheckBox_);

    fileEnabledCheckBox_ = new QCheckBox(QStringLiteral("Write logs to a file"), generalGroup);
    generalLayout->addRow(QString(), fileEnabledCheckBox_);

    logFilePathEdit_ = new QLineEdit(generalGroup);
    logFilePathEdit_->setReadOnly(true);
    logFilePathEdit_->setMinimumWidth(0);
    logFilePathEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    browseLogFileButton_ = new QPushButton(QStringLiteral("Browse..."), generalGroup);
    browseLogFileButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    openLogFolderButton_ = new QPushButton(QStringLiteral("Open Folder"), generalGroup);
    openLogFolderButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    auto *logFileRow = new QWidget(generalGroup);
    logFileRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *logFileLayout = new QHBoxLayout(logFileRow);
    logFileLayout->setContentsMargins(0, 0, 0, 0);
    logFileLayout->setSpacing(6);
    logFileLayout->addWidget(logFilePathEdit_, 1);
    logFileLayout->addWidget(browseLogFileButton_);
    logFileLayout->addWidget(openLogFolderButton_);
    generalLayout->addRow(QStringLiteral("Log File"), logFileRow);

    connect(browseLogFileButton_, &QPushButton::clicked, this, &LoggingSettingsDialog::browseForLogFile);
    connect(openLogFolderButton_, &QPushButton::clicked, this, &LoggingSettingsDialog::openLogFolder);
    connect(logFilePathEdit_, &QLineEdit::textChanged, this, [this]() { updateLogFileControls(); });

    mainLayout->addWidget(generalGroup);

    auto *categoriesGroup = new QGroupBox(QStringLiteral("Categories"), this);
    auto *categoriesLayout = new QVBoxLayout(categoriesGroup);
    categoriesLayout->setContentsMargins(12, 12, 12, 12);
    for (const auto &category : AppLogger::categories()) {
        auto *checkBox = new QCheckBox(category.label, categoriesGroup);
        checkBox->setToolTip(category.id);
        categoryCheckBoxes_.insert(category.id, checkBox);
        categoriesLayout->addWidget(checkBox);
    }
    mainLayout->addWidget(categoriesGroup);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *applyButton = buttonBox->addButton(QStringLiteral("Apply"), QDialogButtonBox::ApplyRole);
    auto *restoreDefaultsButton = buttonBox->addButton(QStringLiteral("Restore Defaults"), QDialogButtonBox::ResetRole);

    connect(applyButton, &QPushButton::clicked, this, &LoggingSettingsDialog::applySettings);
    connect(restoreDefaultsButton, &QPushButton::clicked, this, &LoggingSettingsDialog::restoreDefaults);
    connect(buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        applySettings();
        accept();
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    loadSettingsIntoWidgets(AppLogger::settings());
}

void LoggingSettingsDialog::applySettings() {
    AppLogger::setSettings(collectSettingsFromWidgets(), true);
    loadSettingsIntoWidgets(AppLogger::settings());
}

void LoggingSettingsDialog::browseForLogFile() {
    const QString currentPath = logFilePathEdit_->text().trimmed();
    const QString initialPath = currentPath.isEmpty() ? AppLogger::defaultLogFilePath() : currentPath;
    const QString selectedPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Select Log File"),
        initialPath,
        QStringLiteral("Log Files (*.log);;All Files (*)"));

    if (selectedPath.isEmpty()) {
        return;
    }

    logFilePathEdit_->setText(QDir::toNativeSeparators(QDir::cleanPath(selectedPath)));
}

void LoggingSettingsDialog::openLogFolder() {
    const QString directoryPath = resolvedLogDirectoryPath();
    if (directoryPath.isEmpty()) {
        return;
    }

    QString pathToOpen = directoryPath;
    QDir directory(pathToOpen);
    if (!directory.exists() && !QDir().mkpath(pathToOpen)) {
        pathToOpen = nearestExistingDirectoryPath(pathToOpen);
    }

    if (pathToOpen.isEmpty()
        || !QDesktopServices::openUrl(QUrl::fromLocalFile(pathToOpen))) {
        QMessageBox::warning(
            this,
            QStringLiteral("Open Folder Failed"),
            QStringLiteral("The log folder could not be opened."));
    }
}

void LoggingSettingsDialog::restoreDefaults() {
    loadSettingsIntoWidgets(AppLogger::defaultSettings());
}

void LoggingSettingsDialog::configureMinimumLevelComboBox() const {
    minimumLevelComboBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    minimumLevelComboBox_->setMaxVisibleItems(5);
    minimumLevelComboBox_->setStyleSheet(
        "QComboBox { combobox-popup: 0; }"
        "QComboBox QAbstractItemView { padding: 0px; margin: 0px; outline: 0; }"
        "QComboBox QAbstractItemView::item { margin: 0px; padding: 0px 4px; min-height: 0px; }");

    auto *view = new QListView(minimumLevelComboBox_);
    view->setUniformItemSizes(true);
    view->setSpacing(0);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    view->setTextElideMode(Qt::ElideNone);
    minimumLevelComboBox_->setView(view);

    const int comboBoxWidth = minimumLevelComboBoxWidth();
    minimumLevelComboBox_->setMinimumWidth(comboBoxWidth);
    minimumLevelComboBox_->view()->setMinimumWidth(comboBoxWidth);
}

void LoggingSettingsDialog::loadSettingsIntoWidgets(const LogSettings &settings) {
    const int levelIndex = minimumLevelComboBox_->findData(static_cast<int>(settings.minimumLevel));
    minimumLevelComboBox_->setCurrentIndex(levelIndex >= 0 ? levelIndex : 0);
    consoleEnabledCheckBox_->setChecked(settings.consoleEnabled);
    fileEnabledCheckBox_->setChecked(settings.fileEnabled);
    logFilePathEdit_->setText(settings.logFilePath);
    updateLogFileControls();

    for (const auto &category : AppLogger::categories()) {
        if (auto *checkBox = categoryCheckBoxes_.value(category.id, nullptr)) {
            checkBox->setChecked(settings.categoryEnabled.value(category.id, true));
        }
    }
}

LogSettings LoggingSettingsDialog::collectSettingsFromWidgets() const {
    LogSettings settings = AppLogger::settings();
    settings.minimumLevel = static_cast<LogLevel>(minimumLevelComboBox_->currentData().toInt());
    settings.consoleEnabled = consoleEnabledCheckBox_->isChecked();
    settings.fileEnabled = fileEnabledCheckBox_->isChecked();
    settings.logFilePath = logFilePathEdit_->text();

    for (const auto &category : AppLogger::categories()) {
        if (const auto *checkBox = categoryCheckBoxes_.value(category.id, nullptr)) {
            settings.categoryEnabled.insert(category.id, checkBox->isChecked());
        }
    }

    return settings;
}

int LoggingSettingsDialog::minimumLevelComboBoxWidth() const {
    QString longestLabel;
    int widestLabel = 0;
    const QFontMetrics fontMetrics(minimumLevelComboBox_->font());
    for (int index = 0; index < minimumLevelComboBox_->count(); ++index) {
        const QString label = minimumLevelComboBox_->itemText(index);
        const int labelWidth = fontMetrics.horizontalAdvance(label);
        if (labelWidth > widestLabel) {
            widestLabel = labelWidth;
            longestLabel = label;
        }
    }

    minimumLevelComboBox_->setMinimumContentsLength(longestLabel.size());

    QStyleOptionComboBox option;
    option.initFrom(minimumLevelComboBox_);
    option.editable = minimumLevelComboBox_->isEditable();
    option.currentText = longestLabel;

    const QSize contentSize(widestLabel, fontMetrics.height());
    return minimumLevelComboBox_->style()
        ->sizeFromContents(QStyle::CT_ComboBox, &option, contentSize, minimumLevelComboBox_)
        .width();
}

QString LoggingSettingsDialog::resolvedLogDirectoryPath() const {
    QString candidatePath = logFilePathEdit_->text().trimmed();
    if (candidatePath.isEmpty()) {
        candidatePath = AppLogger::defaultLogFilePath();
    }

    const QFileInfo fileInfo(candidatePath);
    QString directoryPath = fileInfo.isDir() ? fileInfo.absoluteFilePath() : fileInfo.absolutePath();
    if (directoryPath.isEmpty()) {
        directoryPath = QFileInfo(AppLogger::defaultLogFilePath()).absolutePath();
    }
    return directoryPath;
}

void LoggingSettingsDialog::updateLogFileControls() {
    if (openLogFolderButton_ != nullptr) {
        openLogFolderButton_->setEnabled(!resolvedLogDirectoryPath().isEmpty());
    }
}
