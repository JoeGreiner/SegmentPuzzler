#include "AboutDialog.h"
#include "SegmentPuzzlerBuildInfo.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QVTKOpenGLNativeWidget.h>
#include <itkVersion.h>
#include <vtkNew.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkVersion.h>

namespace {

QString renderInfoText() {
    static const QString info = []() {
        // Use the viewer's Qt/VTK path and retain only the result, once per process.
        QVTKOpenGLNativeWidget probe;
        probe.resize(1, 1);
        vtkNew<vtkRenderer> renderer;
        probe.renderWindow()->AddRenderer(renderer);
        // Initializes the hidden widget without showing a window.
        if (probe.grabFramebuffer().isNull()) {
            return AboutDialog::tr("3D renderer: unavailable (could not initialize the test context)");
        }
        probe.makeCurrent();
        auto *functions = probe.context()->functions();
        const auto value = [functions](GLenum name) {
            const auto *text = functions->glGetString(name);
            return text ? QString::fromLatin1(reinterpret_cast<const char *>(text))
                        : AboutDialog::tr("unavailable");
        };
        return AboutDialog::tr("3D context: automatic test context\n"
                               "Renderer: %1\nOpenGL vendor: %2\nOpenGL version: %3")
            .arg(value(GL_RENDERER), value(GL_VENDOR), value(GL_VERSION));
    }();
    return info;
}

QString buildInfoText() {
    QStringList lines{
        QStringLiteral("SegmentPuzzler %1").arg(QApplication::applicationVersion()),
        QStringLiteral("Git: %1").arg(QString::fromUtf8(segment_puzzler::gitDescription)),
        QStringLiteral("Qt: %1").arg(QString::fromLatin1(qVersion())),
        QStringLiteral("VTK: %1").arg(QString::fromLatin1(vtkVersion::GetVTKVersion())),
        QStringLiteral("ITK: %1").arg(QString::fromLatin1(itk::Version::GetITKVersion())),
        QStringLiteral("System: %1 (%2)")
            .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()),
        QStringLiteral("Kernel: %1 %2").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion()),
        QStringLiteral("Qt platform: %1").arg(QGuiApplication::platformName())
    };
    if (QString::fromLatin1(qVersion()) != QStringLiteral(QT_VERSION_STR)) {
        lines.insert(3, QStringLiteral("Qt build: %1").arg(QStringLiteral(QT_VERSION_STR)));
    }

#ifdef Q_OS_LINUX
    // The actual Qt platform and the session's environment are separate facts:
    // xcb can be used in either an X11 session or through Xwayland.
    for (const auto *name : {"XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DESKTOP_SESSION",
                             "DISPLAY", "WAYLAND_DISPLAY"}) {
        const auto value = qEnvironmentVariable(name);
        lines.append(QStringLiteral("%1: %2").arg(QString::fromLatin1(name),
            value.isEmpty() ? AboutDialog::tr("not set") : value));
    }
#endif
    lines.append(QString());
    lines.append(renderInfoText());

    QStringList environment;
    // Only collect graphics/session settings, never the full environment.
    for (const auto *name : {"QT_QPA_PLATFORM", "QT_XCB_GL_INTEGRATION", "QT_OPENGL",
                             "LIBGL_ALWAYS_SOFTWARE", "LIBGL_ALWAYS_INDIRECT",
                             "MESA_LOADER_DRIVER_OVERRIDE", "GALLIUM_DRIVER", "DRI_PRIME",
                             "__NV_PRIME_RENDER_OFFLOAD", "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
                             "__GLX_VENDOR_LIBRARY_NAME", "VGL_DISPLAY", "VGL_TRANSPORT",
                             "XRDP_SESSION", "XDG_SESSION_REMOTE", "LD_PRELOAD", "LD_LIBRARY_PATH"}) {
        if (qEnvironmentVariableIsSet(name)) {
            environment.append(QStringLiteral("%1=%2")
                .arg(QString::fromLatin1(name), qEnvironmentVariable(name)));
        }
    }
    if (!environment.isEmpty()) {
        lines.append(QString());
        lines.append(AboutDialog::tr("Graphics/session environment:"));
        lines.append(environment);
    }
    return lines.join(QLatin1Char('\n'));
}

QString releaseUpdateText(const QByteArray &json, const QString &currentVersion) {
    const QJsonObject release = QJsonDocument::fromJson(json).object();
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    const QRegularExpression versionPattern(QStringLiteral("^v?([0-9]+\\.[0-9]+\\.[0-9]+)$"));
    const auto match = versionPattern.match(tag);
    const QVersionNumber latest = QVersionNumber::fromString(match.captured(1));
    const QVersionNumber current = QVersionNumber::fromString(currentVersion);
    if (!match.hasMatch() || latest.segmentCount() != 3 || current.isNull() ||
        release.value(QStringLiteral("draft")).toBool(true) ||
        release.value(QStringLiteral("prerelease")).toBool(true)) {
        return AboutDialog::tr("Could not read the latest release version.");
    }

    if (latest > current) {
        return AboutDialog::tr(
            "Version %1 is available. "
            "<a href=\"https://github.com/JoeGreiner/SegmentPuzzler/releases/tag/%2\">View release</a>")
            .arg(latest.toString(), QString::fromLatin1(QUrl::toPercentEncoding(tag)));
    }
    return AboutDialog::tr("No newer release available (latest: %1).")
        .arg(latest.toString());
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("About SegmentPuzzler"));
    setAttribute(Qt::WA_DeleteOnClose);

    const QString version = QApplication::applicationVersion();
    const QString infoText = buildInfoText();

    auto *layout = new QVBoxLayout(this);
    auto *heading = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setPixmap(windowIcon().pixmap(64, 64));
    heading->addWidget(icon);
    auto *title = new QLabel(tr("SegmentPuzzler %1").arg(version), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    heading->addWidget(title, 1);
    layout->addLayout(heading);

    auto *buildInfo = new QPlainTextEdit(this);
    buildInfo->setObjectName(QStringLiteral("buildInfo"));
    buildInfo->setReadOnly(true);
    buildInfo->setFont(font());
    buildInfo->setMinimumWidth(540);
    buildInfo->setMinimumHeight(240);
    buildInfo->setPlainText(infoText.section('\n', 1));
    buildInfo->setAccessibleName(tr("Build and rendering information"));
    layout->addWidget(buildInfo, 1);

    auto *description = new QLabel(
        tr("Interactive editing and proofreading of 3D instance segmentations."), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *credits = new QLabel(tr(
        "© 2019 Joachim Greiner · MIT License<br>"
        "<a href=\"https://github.com/JoeGreiner/SegmentPuzzler\">GitHub</a>"), this);
    credits->setOpenExternalLinks(true);
    layout->addWidget(credits);

    auto *citationRow = new QHBoxLayout;
    auto *citation = new QLabel(this);
    citation->ensurePolished();
    citation->setText(tr(
        "If you use SegmentPuzzler in your research, please consider citing "
        "<a href=\"https://doi.org/10.1113/JP288557\" style=\"color: %1;\">Greiner et al. (2026)</a>.")
        .arg(citation->palette().color(QPalette::WindowText).name()));
    citation->setWordWrap(true);
    citation->setOpenExternalLinks(true);
    citationRow->addWidget(citation, 1);
    auto *cite = new QPushButton(tr("How to cite…"), this);
    connect(cite, &QPushButton::clicked, this, &AboutDialog::citationRequested);
    citationRow->addWidget(cite);
    layout->addLayout(citationRow);

    auto *status = new QLabel(this);
    status->setTextFormat(Qt::RichText);
    status->setWordWrap(true);
    status->setOpenExternalLinks(true);
    layout->addWidget(status);
    status->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy = buttons->addButton(tr("Copy build info"), QDialogButtonBox::ActionRole);
    auto *check = buttons->addButton(tr("Check for updates"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(copy, &QPushButton::clicked, this, [infoText]() {
        QApplication::clipboard()->setText(infoText);
    });
    layout->addWidget(buttons);

    auto *network = new QNetworkAccessManager(this);
    connect(check, &QPushButton::clicked, this, [this, network, status, check, version]() {
        check->setEnabled(false);
        status->setText(tr("Checking for updates…"));
        status->show();
        QNetworkRequest request(QUrl(QStringLiteral(
            "https://api.github.com/repos/JoeGreiner/SegmentPuzzler/releases/latest")));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("User-Agent", "SegmentPuzzler/" + version.toUtf8());
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = network->get(request);
        QTimer::singleShot(10000, reply, [reply]() {
            if (reply->isRunning()) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, status, check, version]() {
            check->setEnabled(true);
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (httpStatus == 404) {
                status->setText(tr("No published release found."));
            } else if (reply->error() != QNetworkReply::NoError || httpStatus != 200) {
                status->setText(tr("Could not check for updates. Please try again."));
            } else {
                status->setText(releaseUpdateText(reply->readAll(), version));
            }
            reply->deleteLater();
        });
    });
}
