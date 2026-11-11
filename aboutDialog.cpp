#include "aboutDialog.h"
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
#include <QPushButton>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace {

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
    const QString gitDescription = QString::fromUtf8(segment_puzzler::gitDescription);
    const QString buildInfo = QStringLiteral("SegmentPuzzler %1\nGit: %2\nQt: %3\nSystem: %4 (%5)")
        .arg(version, gitDescription, QString::fromLatin1(qVersion()),
             QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());

    auto *layout = new QVBoxLayout(this);
    auto *heading = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setPixmap(windowIcon().pixmap(64, 64));
    heading->addWidget(icon);
    auto *details = new QVBoxLayout;
    auto *title = new QLabel(tr("SegmentPuzzler %1").arg(version), this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    details->addWidget(title);
    auto *buildInfoLabel = new QLabel(buildInfo.section('\n', 1), this);
    buildInfoLabel->setTextFormat(Qt::PlainText);
    buildInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(buildInfoLabel);
    heading->addLayout(details, 1);
    layout->addLayout(heading);

    auto *description = new QLabel(
        tr("Interactive editing and proofreading of 3D instance segmentations."), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *credits = new QLabel(tr(
        "© 2019 Joachim Greiner · MIT License<br>"
        "<a href=\"https://github.com/JoeGreiner/SegmentPuzzler\">GitHub</a>"), this);
    credits->setOpenExternalLinks(true);
    layout->addWidget(credits);

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
    connect(copy, &QPushButton::clicked, this, [buildInfo]() {
        QApplication::clipboard()->setText(buildInfo);
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
