#include "src/qtUtils/GraphicsSettings.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int simulateRenderTest(const QStringList &arguments) {
    const bool automatic = arguments.contains(QStringLiteral("--graphics=auto"));
    QFile calls(qEnvironmentVariable("SP_GRAPHICS_TEST_CALLS"));
    if (!calls.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return 2;
    }
    calls.write(automatic ? "auto\n" : "system\n");
    calls.close();
    int platformIndex = arguments.indexOf(QStringLiteral("-platform"));
    if (platformIndex < 0) {
        platformIndex = arguments.indexOf(QStringLiteral("--platform"));
    }
    const QString platform = platformIndex >= 0 ? arguments.value(platformIndex + 1) : QString();
    if (qEnvironmentVariableIsSet("SP_GRAPHICS_TEST_EXPECT_PLATFORM") &&
        platform != qEnvironmentVariable("SP_GRAPHICS_TEST_EXPECT_PLATFORM")) {
        return 4;
    }
    if (qEnvironmentVariableIsSet("SP_GRAPHICS_TEST_EXPECT_QPA") &&
        qgetenv("QT_QPA_PLATFORM") != qgetenv("SP_GRAPHICS_TEST_EXPECT_QPA")) {
        return 5;
    }
    if (qEnvironmentVariableIsSet("SP_GRAPHICS_TEST_EXPECT_DISPLAY") &&
        arguments.value(arguments.indexOf(QStringLiteral("-display")) + 1) !=
            qEnvironmentVariable("SP_GRAPHICS_TEST_EXPECT_DISPLAY")) {
        return 6;
    }
    if (qEnvironmentVariableIsSet("SP_GRAPHICS_TEST_EXPECT_DRI") &&
        (!qEnvironmentVariableIsSet("DRI_PRIME") ||
         qgetenv("DRI_PRIME") != qgetenv("SP_GRAPHICS_TEST_EXPECT_DRI"))) {
        return 3;
    }
    const auto behavior = qgetenv("SP_GRAPHICS_TEST_BEHAVIOR");
    if (behavior == "platform-fail" && !platform.isEmpty()) {
        return 1;
    }
    if (behavior == "hang" || (behavior == "timeout" && automatic)) {
        QThread::sleep(60);
    }
    if (behavior == "crash" && automatic) {
        std::raise(SIGTERM);
        return 1;
    }
    return behavior == "fail" || (behavior == "fallback" && automatic) ? 1 : 0;
}

QByteArray readCalls() {
    QFile calls(qEnvironmentVariable("SP_GRAPHICS_TEST_CALLS"));
    if (!calls.open(QIODevice::ReadOnly)) {
        return {};
    }
    return calls.readAll();
}

void checkDialog(const QByteArray &behavior, const QString &initialMode, const QString &choice,
                 const QString &expectedMode, const QByteArray &expectedCalls, bool cancel = false,
                 const QString &initialPlatform = "auto", const QString &choicePlatform = "auto",
                 const QString &expectedPlatform = "auto") {
    QSettings().setValue(QStringLiteral("graphics/mode"), initialMode);
    QSettings().setValue(QStringLiteral("graphics/platform"), initialPlatform);
    QFile::remove(qEnvironmentVariable("SP_GRAPHICS_TEST_CALLS"));
    qputenv("SP_GRAPHICS_TEST_BEHAVIOR", behavior);
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer driver;
    bool clicked = false;
    bool watchdogExpired = false;
    bool fallbackNotice = false;
    QObject::connect(&driver, &QTimer::timeout, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
            fallbackNotice = true;
            message->accept();
            return;
        }
        if (elapsed.elapsed() > 22000) {
            watchdogExpired = true;
            dialog->reject();
            return;
        }
        auto *combo = dialog->findChild<QComboBox *>(QStringLiteral("graphicsMode"));
        auto *platform = dialog->findChild<QComboBox *>(QStringLiteral("graphicsPlatform"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("graphicsTestStatus"));
        if (!clicked) {
            clicked = true;
            combo->setCurrentIndex(combo->findData(choice));
            platform->setCurrentIndex(platform->findData(choicePlatform));
            buttons->button(QDialogButtonBox::Ok)->click();
        } else if ((cancel && !readCalls().isEmpty()) ||
                   status->text().contains(QStringLiteral("Settings were not changed"))) {
            dialog->reject();
        }
    });
    driver.start(20);
    graphicsSettings::showDialog(nullptr);
    driver.stop();
    require(!watchdogExpired, "Dialog did not complete");
    require(QSettings().value(QStringLiteral("graphics/mode")).toString() == expectedMode,
            "Dialog persisted an unexpected mode");
    require(QSettings().value(QStringLiteral("graphics/platform")).toString() == expectedPlatform,
            "Dialog persisted an unexpected platform");
    require(readCalls() == expectedCalls, "Unexpected number/order of graphics tests");
    require(!cancel || elapsed.elapsed() < 5000, "Cancelling the render test blocked");
    if (behavior == "fallback" || behavior == "crash" || behavior == "timeout" || behavior == "platform-fail") {
        require(fallbackNotice, "Fallback was not explained to the user");
    }
}

} // namespace

int main(int argc, char **argv) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    if (arguments.contains(QStringLiteral("--test-graphics"))) {
        return simulateRenderTest(arguments);
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("SegmentPuzzlerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("GraphicsSettings"));
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    qunsetenv("SEGMENTPUZZLER_GPU");
    qunsetenv("APPDIR");
    qputenv("SP_GRAPHICS_TEST_CALLS", QFile::encodeName(directory.filePath(QStringLiteral("calls"))));

    try {
        require(directory.isValid(), "Could not create isolated settings directory");
        const QStringList command{QStringLiteral("SegmentPuzzler")};
        auto options = graphicsSettings::readStartupOptions(command);
        require(options.mode == "auto" && options.source == "default" && options.platform == "auto", "Wrong defaults");
        require(graphicsSettings::saveSettings("system", "xcb").isEmpty(), "Saving settings failed");
        options = graphicsSettings::readStartupOptions(command);
        require(options.mode == "system" && options.source == "settings" && options.platform == "xcb",
                "Saved settings ignored");
        require(!graphicsSettings::saveSettings("system", "invalid").isEmpty(), "Invalid platform accepted");
        require(QSettings().value(QStringLiteral("graphics/platform")) == "xcb", "Invalid save changed settings");
        qputenv("SEGMENTPUZZLER_GPU", "auto");
        options = graphicsSettings::readStartupOptions(command);
        require(options.mode == "auto" && options.source == "environment", "Environment priority failed");
        options = graphicsSettings::readStartupOptions(command + QStringList{"--graphics=system"});
        require(options.mode == "system" && options.source == "command line", "CLI priority failed");
        options = graphicsSettings::readStartupOptions(command + QStringList{"--graphics", "system"});
        require(options.mode == "system", "Separate CLI argument failed");
        qputenv("SEGMENTPUZZLER_GPU", "invalid");
        require(!graphicsSettings::readStartupOptions(command).error.isEmpty(), "Invalid environment accepted");
        options = graphicsSettings::readStartupOptions(command + QStringList{"--reset-graphics"});
        require(options.reset && options.error.isEmpty(), "Invalid environment prevents reset");
        qunsetenv("SEGMENTPUZZLER_GPU");
        QSettings().setValue(QStringLiteral("graphics/mode"), QStringLiteral("invalid"));
        options = graphicsSettings::readStartupOptions(command);
        require(options.mode == "system" && !options.warning.isEmpty() && options.error.isEmpty(),
                "Invalid stored mode did not fall back");
        QSettings().setValue(QStringLiteral("graphics/platform"), QStringLiteral("invalid"));
        options = graphicsSettings::readStartupOptions(command);
        require(options.platform == "auto" && options.warning.contains("platform"), "Invalid platform did not fall back");
        require(!graphicsSettings::readStartupOptions(command + QStringList{"--graphics=invalid"}).error.isEmpty(),
                "Invalid explicit mode accepted");
        require(!graphicsSettings::readStartupOptions(command + QStringList{"--graphics"}).error.isEmpty(),
                "Missing graphics argument accepted");
        QSettings().setValue(QStringLiteral("other/preference"), 42);
        require(graphicsSettings::resetSettings().isEmpty(), "Reset failed");
        require(!QSettings().contains(QStringLiteral("graphics/mode")) &&
                    !QSettings().contains(QStringLiteral("graphics/platform")), "Reset retained graphics keys");
        require(QSettings().value(QStringLiteral("other/preference")).toInt() == 42, "Reset changed other preferences");
        const QString blockedPath = directory.filePath(QStringLiteral("not-a-directory"));
        QFile blocker(blockedPath);
        require(blocker.open(QIODevice::WriteOnly), "Could not prepare settings failure");
        blocker.close();
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, blockedPath);
        require(!graphicsSettings::saveSettings("system", "xcb").isEmpty(), "Settings write failure was not reported");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        options = graphicsSettings::readStartupOptions(command + QStringList{
            "--graphics=system", "-platform", "xcb", "-display", ":12", "--test-graphics"});
        require(options.test && options.qtArguments == QStringList{"-platform", "xcb", "-display", ":12"},
                "Test lost explicit Qt platform/display arguments");
        graphicsSettings::applyStartupOptions(options);
        graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(command));

#ifdef Q_OS_LINUX
        qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
        qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
        qputenv("DRI_PRIME", "1");
        qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
        graphicsSettings::applyStartupOptions(options);
        require(!qEnvironmentVariableIsSet("__NV_PRIME_RENDER_OFFLOAD") &&
                    !qEnvironmentVariableIsSet("__GLX_VENDOR_LIBRARY_NAME") && !qEnvironmentVariableIsSet("DRI_PRIME"),
                "System mode retained GPU selectors");
        require(qgetenv("LIBGL_ALWAYS_SOFTWARE") == "1", "System mode changed unrelated rendering settings");
        qunsetenv("LIBGL_ALWAYS_SOFTWARE");
        graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(command));
#endif

        for (const char *selector : {
                 "pci-0000_00_02_0",
#ifndef Q_OS_WIN
                 "", // qputenv removes empty values on Windows.
#endif
        }) {
            qputenv("DRI_PRIME", selector);
            graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(
                command + QStringList{"--graphics=system"}));
            // Reproduce the environment cleanup performed by Linux system mode.
            qunsetenv("DRI_PRIME");
            qputenv("SP_GRAPHICS_TEST_EXPECT_DRI", selector);
            checkDialog("success", "system", "auto", "auto", "auto\n");
            require(!qEnvironmentVariableIsSet("DRI_PRIME"), "Test changed the parent GPU environment");
        }
        qunsetenv("SP_GRAPHICS_TEST_EXPECT_DRI");
        graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(command));

        checkDialog("success", "auto", "system", "system", "system\n");
        checkDialog("success", "system", "system", "system", "");
        checkDialog("success", "invalid", "system", "system", "system\n");
        checkDialog("fallback", "system", "auto", "system", "auto\nsystem\n");
        checkDialog("crash", "system", "auto", "system", "auto\nsystem\n");
        checkDialog("fail", "system", "auto", "system", "auto\nsystem\n");
        checkDialog("timeout", "system", "auto", "system", "auto\nsystem\n");
        checkDialog("hang", "auto", "system", "auto", "system\n", true);

        // Test and save both graphics settings when the window system changes.
        for (const auto *platform : {"xcb", "wayland"}) {
            qputenv("SP_GRAPHICS_TEST_EXPECT_PLATFORM", platform);
            checkDialog("success", "system", "system", "system", "system\n", false, "auto", platform, platform);
        }
        qunsetenv("SP_GRAPHICS_TEST_EXPECT_PLATFORM");
        checkDialog("success", "system", "system", "system", "", false, "xcb", "xcb", "xcb");
        checkDialog("success", "system", "system", "system", "system\n", false, "invalid", "auto", "auto");
        checkDialog("platform-fail", "system", "system", "system", "system\nsystem\n", false, "auto", "wayland", "auto");
        checkDialog("fail", "system", "auto", "system", "auto\nsystem\n", false, "xcb", "wayland", "xcb");
        checkDialog("hang", "system", "system", "system", "system\n", true, "xcb", "wayland", "xcb");

        // Automatic selection restores the launch environment.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(command));
        qputenv("QT_QPA_PLATFORM", "xcb");
        qputenv("SP_GRAPHICS_TEST_EXPECT_QPA", "offscreen");
        checkDialog("success", "system", "system", "system", "system\n", false, "xcb", "auto", "auto");
        require(qgetenv("QT_QPA_PLATFORM") == "xcb", "Test changed the parent Qt environment");
        qunsetenv("SP_GRAPHICS_TEST_EXPECT_QPA");
        qputenv("QT_QPA_PLATFORM", "offscreen");

        // Explicit choices replace a launch-time platform argument, while preserving the display.
        graphicsSettings::applyStartupOptions(graphicsSettings::readStartupOptions(
            command + QStringList{"-platform", "wayland", "-display", ":12"}));
        qputenv("SP_GRAPHICS_TEST_EXPECT_PLATFORM", "xcb");
        qputenv("SP_GRAPHICS_TEST_EXPECT_DISPLAY", ":12");
        checkDialog("success", "system", "system", "system", "system\n", false, "auto", "xcb", "xcb");
        qputenv("SP_GRAPHICS_TEST_EXPECT_PLATFORM", "wayland");
        checkDialog("success", "system", "system", "system", "system\n", false, "xcb", "auto", "auto");
        qunsetenv("SP_GRAPHICS_TEST_EXPECT_PLATFORM");
        qunsetenv("SP_GRAPHICS_TEST_EXPECT_DISPLAY");

        // Saved platform choices apply on Linux when QT_QPA_PLATFORM is unset.
        options = graphicsSettings::readStartupOptions(command);
        options.platform = QStringLiteral("wayland");
        graphicsSettings::applyStartupOptions(options);
        require(qgetenv("QT_QPA_PLATFORM") == "offscreen", "Saved platform overrode the environment");
        qunsetenv("QT_QPA_PLATFORM");
        graphicsSettings::applyStartupOptions(options);
#ifdef Q_OS_LINUX
        require(qgetenv("QT_QPA_PLATFORM") == "wayland", "Saved platform was not applied on Linux");
#else
        require(!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"), "Linux platform preference affected another OS");
#endif
        qunsetenv("QT_QPA_PLATFORM");
        QSettings().setValue(QStringLiteral("graphics/platform"), QStringLiteral("wayland"));
        options = graphicsSettings::readStartupOptions(command + QStringList{"--test-graphics", "--print-graphics-settings"});
        require(options.test && options.printSettings && options.platform == "auto",
                "Probe/query did not ignore the saved platform");
        graphicsSettings::applyStartupOptions(options);
        require(!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"), "Probe reapplied the saved platform");
        require(QSettings().value(QStringLiteral("graphics/platform")) == "wayland", "Probe changed saved preferences");

        std::cout << "Graphics settings tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
