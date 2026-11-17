#include "GraphicsSettings.h"
#include "src/utils/AppLogger.h"

#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkNew.h>
#include <vtkOpenGLRenderWindow.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSphereSource.h>
#include <limits>

namespace graphicsSettings {
namespace {

constexpr auto modeKey = "graphics/mode";
constexpr auto platformKey = "graphics/platform";
constexpr auto msaaKey = "graphics/forceMsaaOff";
constexpr const char *gpuVariables[] = {
    "DRI_PRIME", "__NV_PRIME_RENDER_OFFLOAD", "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
    "__GLX_VENDOR_LIBRARY_NAME", "__EGL_VENDOR_LIBRARY_FILENAMES", "__EGL_VENDOR_LIBRARY_DIRS"
};
QStringList probeQtArguments;
QProcessEnvironment startupEnvironment;

bool validMode(const QString &mode) {
    return mode == QStringLiteral("auto") || mode == QStringLiteral("system");
}

bool validPlatform(const QString &platform) {
    return platform == QStringLiteral("auto") || platform == QStringLiteral("xcb") ||
           platform == QStringLiteral("wayland");
}

QString writeSettings(const QVariant &mode, const QVariant &platform, const LodSettings &lod, bool forceMsaaOff) {
    QSettings settings;
    struct Setting {
        const char *key;
        QVariant value;
        QVariant previous;
    };
    Setting changes[] = {
        {modeKey, mode, {}}, {platformKey, platform, {}}, {msaaKey, forceMsaaOff, {}},
        {lodEnabledKey, lod.enabled, {}}, {lodThresholdKey, lod.voxelThreshold, {}},
        {lodFactorKey, lod.reductionFactor, {}}
    };
    const auto writeValue = [&](const char *key, const QVariant &value) {
        if (value.isValid()) {
            settings.setValue(QLatin1String(key), value);
        } else {
            settings.remove(QLatin1String(key));
        }
    };
    for (auto &change : changes) {
        change.previous = settings.value(QLatin1String(change.key));
        writeValue(change.key, mode.isValid() ? change.value : QVariant());
    }
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        return {};
    }
    // Restore previous values before QSettings retries a sync.
    for (const auto &change : changes) {
        writeValue(change.key, change.previous);
    }
    return QObject::tr("Could not save graphics settings to %1.").arg(settings.fileName());
}

void restoreStartupVariable(QProcessEnvironment &environment, const char *name) {
    const QString key = QString::fromLatin1(name);
    if (startupEnvironment.contains(key)) {
        environment.insert(key, startupEnvironment.value(key));
    } else {
        environment.remove(key);
    }
}

QString testProgram() {
#ifdef Q_OS_LINUX
    const QString appDir = qEnvironmentVariable("APPDIR");
    const QFileInfo launcher(QDir(appDir).filePath(QStringLiteral("AppRun")));
    const QString executable = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    if (!appDir.isEmpty() && launcher.isExecutable() &&
        executable.startsWith(QDir(appDir).canonicalPath() + QLatin1Char('/'))) {
        return launcher.absoluteFilePath();
    }
#endif
    return QCoreApplication::applicationFilePath();
}

} // namespace

StartupOptions readStartupOptions(const QStringList &arguments) {
    StartupOptions options;
    QString overrideMode;
    bool hasOverride = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments[i];
        if (argument == QStringLiteral("--")) {
            break;
        }
        if (argument.startsWith(QStringLiteral("--graphics="))) {
            overrideMode = argument.mid(11);
            hasOverride = true;
        } else if (argument == QStringLiteral("--graphics")) {
            hasOverride = true;
            overrideMode = ++i < arguments.size() ? arguments[i] : QString();
        } else if (argument == QStringLiteral("--print-graphics-settings")) {
            options.printSettings = true;
        } else if (argument == QStringLiteral("--reset-graphics")) {
            options.reset = true;
        } else if (argument == QStringLiteral("--test-graphics")) {
            options.test = true;
        } else if (argument == QStringLiteral("-platform") || argument == QStringLiteral("--platform") ||
                   argument == QStringLiteral("-display") || argument == QStringLiteral("--display")) {
            options.qtArguments.append(argument);
            if (i + 1 < arguments.size()) {
                options.qtArguments.append(arguments[++i]);
            }
        }
    }
    // Handle reset before validating graphics settings.
    if (options.reset) {
        return options;
    }
    QSettings settings;
    if (hasOverride) {
        options.mode = overrideMode;
        options.source = QStringLiteral("command line");
    } else if (qEnvironmentVariableIsSet("SEGMENTPUZZLER_GPU")) {
        options.mode = qEnvironmentVariable("SEGMENTPUZZLER_GPU");
        options.source = QStringLiteral("environment");
    } else if (settings.contains(QLatin1String(modeKey))) {
        options.mode = settings.value(QLatin1String(modeKey)).toString();
        options.source = QStringLiteral("settings");
    } else {
        options.mode = QStringLiteral("auto");
        options.source = QStringLiteral("default");
    }
    if ((options.source == QStringLiteral("settings") || options.source == QStringLiteral("default")) &&
        settings.status() != QSettings::NoError) {
        options.mode = QStringLiteral("system");
        options.warning = QStringLiteral("Could not read graphics settings from %1. Using system default.")
                              .arg(settings.fileName());
    }
    if (!validMode(options.mode)) {
        const QString error = QStringLiteral("Invalid graphics mode '%1' from %2; expected auto or system.")
                                  .arg(options.mode, options.source);
        if (options.source == QStringLiteral("settings")) {
            options.warning = error + QStringLiteral(" Using system default.");
            options.mode = QStringLiteral("system");
        } else {
            options.error = error;
        }
    }
    // Probes use the platform passed through Qt arguments or the environment.
    if (!options.test && settings.status() == QSettings::NoError) {
        options.platform = settings.value(QLatin1String(platformKey), QStringLiteral("auto")).toString();
        if (!validPlatform(options.platform)) {
            if (!options.warning.isEmpty()) {
                options.warning += QLatin1Char('\n');
            }
            options.warning += QStringLiteral("Invalid stored graphics platform '%1'; using automatic selection.")
                                   .arg(options.platform);
            options.platform = QStringLiteral("auto");
        }
    }
    return options;
}

QString saveSettings(const QString &mode, const QString &platform) {
    return validMode(mode) && validPlatform(platform)
        ? writeSettings(mode, platform, lodSettings(), QSettings().value(QLatin1String(msaaKey), true).toBool())
        : QObject::tr("Invalid graphics settings.");
}

QString resetSettings() {
    return writeSettings(QVariant(), QVariant(), {}, true);
}

void applyStartupOptions(const StartupOptions &options) {
    if (QSettings().value(QLatin1String(msaaKey), true).toBool()) {
        vtkOpenGLRenderWindow::SetGlobalMaximumNumberOfMultiSamples(0);
    }
    probeQtArguments = options.qtArguments;
    startupEnvironment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_LINUX
    if (options.mode == QStringLiteral("system")) {
        for (const char *name : gpuVariables) {
            qunsetenv(name);
        }
    }
    if (options.platform != QStringLiteral("auto") && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", options.platform.toLatin1());
    }
#endif
}

int runRenderTest() {
    QVTKOpenGLNativeWidget widget;
    widget.resize(64, 64);
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkSphereSource> sphere;
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(sphere->GetOutputPort());
    vtkNew<vtkActor> actor;
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(1.0, 0.0, 0.0);
    renderer->AddActor(actor);
    renderer->SetBackground(0.0, 0.0, 0.0);
    renderer->ResetCamera();
    widget.renderWindow()->AddRenderer(renderer);
    const QImage frame = widget.grabFramebuffer();
    if (frame.isNull() || !widget.context() || !widget.context()->isValid() ||
        frame.pixelColor(frame.width() / 2, frame.height() / 2).red() < 16) {
        QTextStream(stderr) << "Qt/VTK graphics test could not render the test geometry.\n";
        return 1;
    }
    widget.makeCurrent();
    auto *functions = widget.context()->functions();
    QTextStream output(stdout);
    output << "Qt platform: " << QGuiApplication::platformName() << '\n';
    for (const auto name : {GL_RENDERER, GL_VENDOR, GL_VERSION}) {
        const auto *value = functions->glGetString(name);
        output << (value ? reinterpret_cast<const char *>(value) : "unavailable") << '\n';
    }
    return 0;
}

void showDialog(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Graphics"));
    auto *layout = new QFormLayout(&dialog);
    auto *mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("graphicsMode"));
    mode->addItem(QObject::tr("Automatic"), QStringLiteral("auto"));
    mode->addItem(QObject::tr("System default"), QStringLiteral("system"));
    const QString initialMode = QSettings().value(QLatin1String(modeKey), QStringLiteral("auto")).toString();
    mode->setCurrentIndex(initialMode == QStringLiteral("auto") ? 0 : 1);
    layout->addRow(QObject::tr("GPU:"), mode);
    auto *platform = new QComboBox(&dialog);
    platform->setObjectName(QStringLiteral("graphicsPlatform"));
    platform->addItem(QObject::tr("Automatic"), QStringLiteral("auto"));
    platform->addItem(QObject::tr("X11 (xcb)"), QStringLiteral("xcb"));
    platform->addItem(QObject::tr("Wayland"), QStringLiteral("wayland"));
    const QString initialPlatform = QSettings().value(QLatin1String(platformKey), QStringLiteral("auto")).toString();
    platform->setCurrentIndex(validPlatform(initialPlatform) ? platform->findData(initialPlatform) : 0);
    layout->addRow(QObject::tr("Window system:"), platform);
    auto *description = new QLabel(QObject::tr(
        "Automatic uses the launcher's GPU preference. System default uses the system's GPU selection.\n"
        "Changes are tested before saving and take effect after restarting SegmentPuzzler."), &dialog);
    description->setWordWrap(true);
    layout->addRow(description);
#ifndef Q_OS_LINUX
    layout->setRowVisible(mode, false);
    layout->setRowVisible(platform, false);
    layout->setRowVisible(description, false);
#endif
    const auto initialLod = lodSettings();
    auto *lodEnabled = new QCheckBox(QObject::tr("Reduce 3D detail (LOD)"), &dialog);
    lodEnabled->setObjectName(QStringLiteral("lodEnabled"));
    lodEnabled->setChecked(initialLod.enabled);
    lodEnabled->setToolTip(QObject::tr("Applies to newly prepared 3D views. Split tools keep full detail."));
    layout->addRow(lodEnabled);
    auto *lodThreshold = new QSpinBox(&dialog);
    lodThreshold->setObjectName(QStringLiteral("lodVoxelThreshold"));
    lodThreshold->setRange(1, std::numeric_limits<int>::max());
    lodThreshold->setGroupSeparatorShown(true);
    lodThreshold->setValue(initialLod.voxelThreshold);
    lodThreshold->setEnabled(initialLod.enabled);
    QObject::connect(lodEnabled, &QCheckBox::toggled, lodThreshold, &QWidget::setEnabled);
    layout->addRow(QObject::tr("LOD from (voxels in ROI):"), lodThreshold);
    auto *lodFactor = new QSpinBox(&dialog);
    lodFactor->setObjectName(QStringLiteral("lodReductionFactor"));
    lodFactor->setRange(1, 64);
    lodFactor->setValue(initialLod.reductionFactor);
    lodFactor->setEnabled(initialLod.enabled);
    lodFactor->setToolTip(QObject::tr("Sample every Nth voxel along each axis. Applies to newly prepared 3D views."));
    QObject::connect(lodEnabled, &QCheckBox::toggled, lodFactor, &QWidget::setEnabled);
    layout->addRow(QObject::tr("LOD downsample factor:"), lodFactor);
    auto *forceMsaaOff = new QCheckBox(QObject::tr("Force MSAA off"), &dialog);
    forceMsaaOff->setObjectName(QStringLiteral("forceMsaaOff"));
    forceMsaaOff->setChecked(QSettings().value(QLatin1String(msaaKey), true).toBool());
    forceMsaaOff->setToolTip(QObject::tr("Disable multisample anti-aliasing. Takes effect after restarting SegmentPuzzler."));
    layout->addRow(forceMsaaOff);
#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsSet("SEGMENTPUZZLER_GPU") || startupEnvironment.contains("QT_QPA_PLATFORM") ||
        probeQtArguments.contains(QStringLiteral("-platform")) || probeQtArguments.contains(QStringLiteral("--platform"))) {
        auto *overrideNotice = new QLabel(QObject::tr(
            "Launch arguments or environment variables override the saved settings."), &dialog);
        overrideNotice->setWordWrap(true);
        layout->addRow(overrideNotice);
    }
#endif
    auto *status = new QLabel(&dialog);
    status->setObjectName(QStringLiteral("graphicsTestStatus"));
    status->setWordWrap(true);
    layout->addRow(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *restoreDefaults = buttons->addButton(QObject::tr("Restore defaults"), QDialogButtonBox::ResetRole);
    QObject::connect(restoreDefaults, &QPushButton::clicked, &dialog, [&]() {
        mode->setCurrentIndex(0);
        platform->setCurrentIndex(0);
        const LodSettings defaults;
        lodEnabled->setChecked(defaults.enabled);
        lodThreshold->setValue(defaults.voxelThreshold);
        lodFactor->setValue(defaults.reductionFactor);
        forceMsaaOff->setChecked(true);
        status->clear();
    });
    layout->addRow(buttons);
    const auto save = [&](const QString &gpu, const QString &windowSystem) {
        return writeSettings(gpu, windowSystem,
                             {lodEnabled->isChecked(), lodThreshold->value(), lodFactor->value()},
                             forceMsaaOff->isChecked());
    };

    auto *process = new QProcess;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Start each probe with the original launch environment.
    for (const char *name : gpuVariables) {
        restoreStartupVariable(environment, name);
    }
    restoreStartupVariable(environment, "QT_QPA_PLATFORM");
    process->setProcessEnvironment(environment);
    QTimer timer;
    timer.setSingleShot(true);
    QString testedMode;
    QString testedPlatform;
    bool timedOut = false;
    bool cancelled = false;
    bool fallback = false;

    const auto startTest = [&](const QString &choice, const QString &platformChoice) {
        testedMode = choice;
        testedPlatform = platformChoice;
        timedOut = false;
        mode->setEnabled(false);
        platform->setEnabled(false);
        restoreDefaults->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        status->setText(QObject::tr("Testing graphics (%1, %2)…").arg(choice, platformChoice));
        QStringList arguments{QStringLiteral("--test-graphics"), QStringLiteral("--graphics=") + choice};
        for (int i = 0; i < probeQtArguments.size(); ++i) {
            const QString &argument = probeQtArguments[i];
            if (platformChoice != QStringLiteral("auto") &&
                (argument == QStringLiteral("-platform") || argument == QStringLiteral("--platform"))) {
                ++i;
            } else {
                arguments.append(argument);
            }
        }
        if (platformChoice != QStringLiteral("auto")) {
            arguments.append({QStringLiteral("-platform"), platformChoice});
        }
        timer.start(15000);
        process->start(testProgram(), arguments);
    };
    const auto finishTest = [&](bool success, const QString &reason) {
        timer.stop();
        if (cancelled) {
            return;
        }
        const QString details = QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();
        const QString errors = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        using segment_puzzler::app_logging::LogLevel;
        SP_LOG(success ? LogLevel::Info : LogLevel::Warning, "app",
               QStringLiteral("Graphics test mode=%1, platform=%2, result=%3; %4; %5; %6")
                   .arg(testedMode, testedPlatform, success ? QStringLiteral("success") : QStringLiteral("failed"),
                        reason, details.left(4096), errors.left(4096)));
        if (!success && (testedMode != QStringLiteral("system") || testedPlatform != QStringLiteral("auto"))) {
            fallback = true;
            SP_LOG_WARNING("app", "Graphics test failed; trying system GPU and automatic window system once");
            QTimer::singleShot(0, &dialog, [&]() {
                if (!cancelled) {
                    startTest(QStringLiteral("system"), QStringLiteral("auto"));
                }
            });
            return;
        }
        if (success) {
            const QString error = save(testedMode, testedPlatform);
            if (error.isEmpty()) {
                SP_LOG_INFO("app", QStringLiteral("Saved graphics mode=%1, platform=%2; applies after restart")
                                       .arg(testedMode, testedPlatform));
                if (fallback) {
                    QMessageBox::information(&dialog, QObject::tr("Graphics"), QObject::tr(
                        "The selected graphics settings failed the test.\n"
                        "System default GPU and automatic window system passed and were saved.\n"
                        "The change takes effect after restarting SegmentPuzzler."));
                }
                dialog.accept();
                return;
            }
            SP_LOG_WARNING("app", error);
            status->setText(error);
        } else {
            status->setText(QObject::tr("Graphics test failed: %1. Settings were not changed. See the log for details.")
                                .arg(reason));
        }
        mode->setEnabled(true);
        platform->setEnabled(true);
        restoreDefaults->setEnabled(true);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
    };
    QObject::connect(process, &QProcess::finished, &dialog,
                     [&](int code, QProcess::ExitStatus exitStatus) {
        const QString reason = timedOut ? QObject::tr("timed out after 15 seconds")
            : exitStatus == QProcess::CrashExit ? QObject::tr("test process crashed")
                                               : QObject::tr("exit code %1").arg(code);
        finishTest(!timedOut && exitStatus == QProcess::NormalExit && code == 0, reason);
    });
    QObject::connect(process, &QProcess::errorOccurred, &dialog, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishTest(false, process->errorString());
        }
    });
    QObject::connect(&timer, &QTimer::timeout, &dialog, [&]() {
        timedOut = true;
        process->kill();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (mode->currentData().toString() == initialMode && platform->currentData().toString() == initialPlatform) {
            const auto error = save(initialMode, initialPlatform);
            if (error.isEmpty()) dialog.accept();
            else status->setText(error);
        } else {
            fallback = false;
            startTest(mode->currentData().toString(), platform->currentData().toString());
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&dialog, &QDialog::rejected, &dialog, [&]() {
        cancelled = true;
    });
    dialog.exec();
    timer.stop();
    // Defer deletion: QProcess's destructor waits for the child process to exit.
    process->disconnect(&dialog);
    if (process->state() == QProcess::NotRunning) {
        delete process;
    } else {
        QObject::connect(process, &QProcess::stateChanged, process, [process](QProcess::ProcessState state) {
            if (state == QProcess::NotRunning) {
                process->deleteLater();
            }
        });
        process->kill();
    }
}

} // namespace graphicsSettings
