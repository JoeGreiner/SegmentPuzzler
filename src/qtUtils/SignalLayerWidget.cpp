#include "SignalLayerWidget.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <iostream>

namespace {

constexpr int kTreeWidthInset = 8;
constexpr int kOuterSpacing = 4;
constexpr int kMetadataSpacing = 8;
constexpr int kTextRowSlack = 2;
const QMargins kRowMargins(4, 3, 8, 3);
const QSize kVisibilityButtonSize(20, 20);
const QSize kColorButtonSize(28, 24);
constexpr auto kAbbreviationMarker = "(...)";
constexpr auto kMinimumTitleSample = "MMMM";

int textWidth(const QFontMetrics &metrics, const QString &text) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return metrics.horizontalAdvance(text);
#else
    return metrics.width(text);
#endif
}

bool debugLayerLayoutEnabled() {
    static const bool enabled = !qgetenv("SEGMENTPUZZLER_DEBUG_LAYER_LAYOUT").isEmpty();
    return enabled;
}

QString debugTreeName(const QTreeWidget *treeWidget) {
    if (treeWidget == nullptr) {
        return QStringLiteral("<null>");
    }
    if (!treeWidget->objectName().isEmpty()) {
        return treeWidget->objectName();
    }
    return QStringLiteral("QTreeWidget@0x%1").arg(reinterpret_cast<quintptr>(treeWidget), 0, 16);
}

class LayerTreeLayoutSyncFilter final : public QObject {
public:
    explicit LayerTreeLayoutSyncFilter(QTreeWidget *treeWidgetIn)
        : QObject(treeWidgetIn), treeWidget(treeWidgetIn) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (treeWidget == nullptr || event == nullptr) {
            return QObject::eventFilter(watched, event);
        }

        switch (event->type()) {
            case QEvent::Resize:
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::Move:
            case QEvent::LayoutRequest:
                SignalLayerWidget::requestHostTreeLayoutSync(treeWidget);
                break;
            default:
                break;
        }

        return QObject::eventFilter(watched, event);
    }

private:
    QPointer<QTreeWidget> treeWidget;
};

QTreeWidget *hostTreeWidgetFor(const QWidget *widget);

QColor blendColor(const QColor &base, const QColor &accent, double accentWeight) {
    const double clampedWeight = std::clamp(accentWeight, 0.0, 1.0);
    const double baseWeight = 1.0 - clampedWeight;
    return QColor::fromRgbF(base.redF() * baseWeight + accent.redF() * clampedWeight,
                            base.greenF() * baseWeight + accent.greenF() * clampedWeight,
                            base.blueF() * baseWeight + accent.blueF() * clampedWeight,
                            base.alphaF() * baseWeight + accent.alphaF() * clampedWeight);
}

QColor stableLayerCardBaseColor(const QWidget *widget) {
    if (widget != nullptr) {
        if (const QTreeWidget *treeWidget = hostTreeWidgetFor(widget)) {
            if (treeWidget->viewport() != nullptr) {
                QColor viewportBase = treeWidget->viewport()->palette().color(QPalette::Base);
                viewportBase.setAlpha(255);
                return viewportBase;
            }
            QColor treeBase = treeWidget->palette().color(QPalette::Base);
            treeBase.setAlpha(255);
            return treeBase;
        }
    }

    QColor appBase = QApplication::palette().color(QPalette::Base);
    appBase.setAlpha(255);
    return appBase;
}

QIcon makeEyeIcon(const QColor &color, bool visible) {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color, 1.5);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(1.6, 4.3, 12.8, 7.4));

    if (visible) {
        painter.setBrush(color);
        painter.drawEllipse(QRectF(6.0, 6.0, 4.0, 4.0));
    } else {
        painter.drawLine(QPointF(2.4, 13.0), QPointF(13.8, 3.6));
    }

    return QIcon(pixmap);
}

QPainterPath heartPathForRect(const QRectF &rect) {
    const qreal left = rect.left();
    const qreal top = rect.top();
    const qreal width = rect.width();
    const qreal height = rect.height();

    QPainterPath path;
    path.moveTo(left + width * 0.50, top + height * 0.90);
    path.cubicTo(left + width * 0.18, top + height * 0.70,
                 left + width * 0.02, top + height * 0.42,
                 left + width * 0.18, top + height * 0.24);
    path.cubicTo(left + width * 0.30, top + height * 0.08,
                 left + width * 0.46, top + height * 0.13,
                 left + width * 0.50, top + height * 0.28);
    path.cubicTo(left + width * 0.54, top + height * 0.13,
                 left + width * 0.70, top + height * 0.08,
                 left + width * 0.82, top + height * 0.24);
    path.cubicTo(left + width * 0.98, top + height * 0.42,
                 left + width * 0.82, top + height * 0.70,
                 left + width * 0.50, top + height * 0.90);
    path.closeSubpath();
    return path;
}

void drawHeartChrome(QPainter &painter, const QPainterPath &heart) {
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(15, 23, 42, 110), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(heart.translated(0.0, 0.55));
    painter.setPen(QPen(QColor(248, 250, 252, 210), 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(heart);
}

QIcon makeSingleColorHeartIcon(const QColor &color) {
    QPixmap pixmap(28, 24);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPainterPath heart = heartPathForRect(QRectF(2.0, 1.0, 24.0, 21.5));

    painter.save();
    painter.setClipPath(heart);
    painter.fillPath(heart, color);

    QLinearGradient sheen(QPointF(7.0, 2.2), QPointF(20.5, 17.0));
    sheen.setColorAt(0.0, QColor(255, 255, 255, 64));
    sheen.setColorAt(0.42, QColor(255, 255, 255, 18));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.fillRect(QRectF(4.0, 2.0, 18.5, 9.5), sheen);

    QLinearGradient shade(QPointF(13.0, 7.0), QPointF(16.5, 21.0));
    shade.setColorAt(0.0, QColor(0, 0, 0, 0));
    shade.setColorAt(1.0, QColor(15, 23, 42, 58));
    painter.fillRect(QRectF(8.0, 8.0, 13.5, 12.5), shade);
    painter.restore();

    drawHeartChrome(painter, heart);
    return QIcon(pixmap);
}

QIcon makeCategoricalHeartIcon() {
    QPixmap pixmap(28, 24);
    pixmap.fill(Qt::transparent);

    struct SegmentBlob {
        QRectF rect;
        QColor color;
    };

    static const std::array<SegmentBlob, 11> blobs = {{
        {QRectF(4.0, 2.8, 6.0, 5.6), QColor("#ff6b6b")},
        {QRectF(8.1, 3.4, 5.0, 4.6), QColor("#ffd93d")},
        {QRectF(13.1, 2.8, 4.8, 5.0), QColor("#4ade80")},
        {QRectF(17.1, 3.4, 5.3, 5.1), QColor("#38bdf8")},
        {QRectF(5.1, 7.5, 5.6, 4.9), QColor("#fb7185")},
        {QRectF(10.0, 7.0, 4.8, 5.8), QColor("#f97316")},
        {QRectF(14.3, 7.7, 5.5, 5.1), QColor("#818cf8")},
        {QRectF(18.5, 8.3, 3.8, 5.0), QColor("#facc15")},
        {QRectF(7.1, 12.5, 5.4, 5.2), QColor("#2dd4bf")},
        {QRectF(11.8, 12.1, 5.8, 6.0), QColor("#ec4899")},
        {QRectF(16.5, 13.2, 4.8, 4.6), QColor("#60a5fa")}
    }};

    static const std::array<qreal, 11> radii = {
        3.0, 2.5, 2.4, 2.7, 2.6, 2.8, 2.7, 2.2, 2.9, 3.0, 2.5
    };

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPainterPath heart = heartPathForRect(QRectF(2.0, 1.0, 24.0, 21.5));
    painter.fillPath(heart, QColor(255, 255, 255, 34));

    painter.save();
    painter.setClipPath(heart);
    painter.setPen(Qt::NoPen);
    for (size_t index = 0; index < blobs.size(); ++index) {
        painter.setBrush(blobs[index].color);
        painter.drawRoundedRect(blobs[index].rect, radii[index], radii[index]);
    }

    painter.setBrush(QColor(255, 255, 255, 42));
    painter.drawEllipse(QRectF(7.2, 4.2, 3.0, 2.2));
    painter.drawEllipse(QRectF(14.7, 4.6, 2.6, 2.0));
    painter.drawEllipse(QRectF(9.4, 12.9, 2.5, 1.9));
    painter.restore();

    drawHeartChrome(painter, heart);
    return QIcon(pixmap);
}

QIcon makeEdgeStatusIcon() {
    QPixmap pixmap(24, 20);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const std::array<QColor, 3> colors = {
        QColor("#f8fafc"),
        QColor("#ef4444"),
        QColor("#22c55e")
    };
    const std::array<qreal, 3> yPositions = {4.0, 10.0, 16.0};

    for (size_t index = 0; index < colors.size(); ++index) {
        QPen pen(colors[index], 2.8, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QPointF(5.2, yPositions[index]), QPointF(18.8, yPositions[index]));
    }

    return QIcon(pixmap);
}

QFont sharedLayerRowTextFont() {
    QFont font = QApplication::font();
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(std::max(1.0, font.pointSizeF() - 1.0));
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(1, font.pixelSize() - 1));
    }
    font.setWeight(QFont::Normal);
    return font;
}

QString abbreviateLayerNameWithMarker(const QString &fullName, const QFontMetrics &metrics, int availableWidth) {
    if (fullName.isEmpty() || availableWidth <= 0) {
        return QString::fromLatin1(kAbbreviationMarker);
    }

    if (textWidth(metrics, fullName) <= availableWidth) {
        return fullName;
    }

    const QString marker = QString::fromLatin1(kAbbreviationMarker);
    if (textWidth(metrics, marker) >= availableWidth) {
        return marker;
    }

    int low = 1;
    int high = fullName.size();
    int best = 1;
    while (low <= high) {
        const int mid = (low + high) / 2;
        QString candidate = fullName.left(mid).trimmed();
        if (candidate.isEmpty()) {
            candidate = fullName.left(mid);
        }
        candidate += marker;

        if (textWidth(metrics, candidate) <= availableWidth) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    QString abbreviated = fullName.left(best).trimmed();
    if (abbreviated.isEmpty()) {
        abbreviated = fullName.left(1);
    }
    return abbreviated + marker;
}

QTreeWidget *hostTreeWidgetFor(const QWidget *widget) {
    const QWidget *current = widget;
    while (current != nullptr) {
        if (auto *treeWidget = qobject_cast<QTreeWidget *>(const_cast<QWidget *>(current))) {
            return treeWidget;
        }
        current = current->parentWidget();
    }
    return nullptr;
}

int rowFixedHeightForFont(const QFontMetrics &metrics) {
    const int titleLineHeight = metrics.height() + kTextRowSlack;
    const int metadataLineHeight = metrics.height() + kTextRowSlack;
    const int textColumnHeight = titleLineHeight + metadataLineHeight + 2;
    const int leftControlHeight = std::max(kVisibilityButtonSize.height(), kColorButtonSize.height());
    return kRowMargins.top() + std::max(leftControlHeight, textColumnHeight) + kRowMargins.bottom();
}

int leftZoneWidth() {
    return kVisibilityButtonSize.width() + kOuterSpacing + kColorButtonSize.width();
}

int minimumTextColumnWidth(const QFontMetrics &metrics, const QString &contrastText, const QString &opacityText) {
    const int titleWidth = textWidth(metrics, QString::fromLatin1(kMinimumTitleSample));
    const int contrastWidth = contrastText.isEmpty() ? 0 : textWidth(metrics, contrastText);
    const int opacityWidth = opacityText.isEmpty() ? 0 : textWidth(metrics, opacityText);
    const int metadataWidth = contrastWidth > 0 && opacityWidth > 0
                                  ? contrastWidth + kMetadataSpacing + opacityWidth
                                  : std::max(contrastWidth, opacityWidth);
    return std::max(titleWidth, metadataWidth);
}

} // namespace

void SignalLayerWidget::configureHostTree(QTreeWidget *treeWidget) {
    if (treeWidget == nullptr) {
        return;
    }

    treeWidget->setAlternatingRowColors(false);
    treeWidget->setUniformRowHeights(true);
    treeWidget->setFrameShape(QFrame::NoFrame);
    treeWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    if (treeWidget->viewport() != nullptr) {
        treeWidget->viewport()->setAutoFillBackground(false);
    }
    if (treeWidget->header() != nullptr) {
        treeWidget->header()->setStretchLastSection(false);
        treeWidget->header()->setSectionResizeMode(0, QHeaderView::Fixed);
        treeWidget->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    }
    treeWidget->setStyleSheet(
        "QTreeWidget { background: transparent; border: 0px; outline: 0px; }"
        "QTreeWidget::item { background: transparent; border: 0px; padding: 0px; margin: 0px; }"
        "QTreeWidget::item:selected { background: transparent; color: palette(text); }"
        "QTreeWidget::item:hover { background: transparent; }"
        "QTreeWidget::branch { background: transparent; }");

    if (!treeWidget->property("signalLayerSyncInstalled").toBool()) {
        auto *syncFilter = new LayerTreeLayoutSyncFilter(treeWidget);
        treeWidget->installEventFilter(syncFilter);
        if (treeWidget->viewport() != nullptr) {
            treeWidget->viewport()->installEventFilter(syncFilter);
        }
        treeWidget->setProperty("signalLayerSyncInstalled", true);
    }
}

void SignalLayerWidget::requestHostTreeLayoutSync(QTreeWidget *treeWidget) {
    if (treeWidget == nullptr) {
        return;
    }

    if (treeWidget->property("signalLayerSyncPending").toBool()) {
        return;
    }

    treeWidget->setProperty("signalLayerSyncPending", true);
    QPointer<QTreeWidget> guardedTree = treeWidget;
    QTimer::singleShot(0, treeWidget, [guardedTree]() {
        if (guardedTree == nullptr) {
            return;
        }
        guardedTree->setProperty("signalLayerSyncPending", false);
        SignalLayerWidget::syncHostTreeLayout(guardedTree);
    });
}

void SignalLayerWidget::syncHostTreeLayout(QTreeWidget *treeWidget) {
    if (treeWidget == nullptr) {
        return;
    }

    const int viewportWidth = treeWidget->viewport() != nullptr ? treeWidget->viewport()->width() : treeWidget->width();
    const int contentColumnWidth = std::max(0, viewportWidth - kTreeWidthInset);
    treeWidget->setColumnWidth(0, contentColumnWidth);
    treeWidget->setColumnWidth(1, 0);

    for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *item = treeWidget->topLevelItem(itemIndex);
        if (item == nullptr) {
            continue;
        }

        auto *layerWidget = qobject_cast<SignalLayerWidget *>(treeWidget->itemWidget(item, 0));
        if (layerWidget == nullptr) {
            continue;
        }

        if (QWidget *spacerWidget = treeWidget->itemWidget(item, 1)) {
            treeWidget->removeItemWidget(item, 1);
            spacerWidget->deleteLater();
        }

        const int resolvedWidth = contentColumnWidth > 0 ? contentColumnWidth : layerWidget->minimumSizeHint().width();
        const QSize contentSize = layerWidget->preferredSizeForWidth(resolvedWidth);
        if (resolvedWidth > 0) {
            layerWidget->setMinimumWidth(resolvedWidth);
            layerWidget->setMaximumWidth(resolvedWidth);
        } else {
            layerWidget->setMinimumWidth(0);
            layerWidget->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        layerWidget->updateDisplayedLayerName();
        item->setSizeHint(0, contentSize);
        item->setSizeHint(1, QSize(0, contentSize.height()));
    }

    treeWidget->doItemsLayout();

    if (!debugLayerLayoutEnabled()) {
        return;
    }

    const QString treeName = debugTreeName(treeWidget);
    const bool scrollbarVisible = treeWidget->verticalScrollBar() != nullptr && treeWidget->verticalScrollBar()->isVisible();
    std::cout << QStringLiteral("[LayerLayoutSync] tree=%1 viewport=%2 column0=%3 items=%4 scrollbarVisible=%5")
                     .arg(treeName)
                     .arg(viewportWidth)
                     .arg(treeWidget->columnWidth(0))
                     .arg(treeWidget->topLevelItemCount())
                     .arg(scrollbarVisible)
                     .toStdString()
              << std::endl;

    for (int itemIndex = 0; itemIndex < treeWidget->topLevelItemCount(); ++itemIndex) {
        QTreeWidgetItem *item = treeWidget->topLevelItem(itemIndex);
        if (item == nullptr) {
            continue;
        }

        auto *layerWidget = qobject_cast<SignalLayerWidget *>(treeWidget->itemWidget(item, 0));
        if (layerWidget == nullptr) {
            continue;
        }

        const QModelIndex modelIndex = treeWidget->model()->index(itemIndex, 0);
        const QRect itemRect = modelIndex.isValid() ? treeWidget->visualRect(modelIndex) : treeWidget->visualItemRect(item);
        const int legacyResolvedWidth = std::max(0, viewportWidth - kTreeWidthInset);
        const int resolvedWidth = contentColumnWidth > 0 ? contentColumnWidth : layerWidget->minimumSizeHint().width();
        const QSize minHint = layerWidget->minimumSizeHint();
        const QSize sizeHint = layerWidget->sizeHint();
        const QSize itemHint = item->sizeHint(0);
        const QRect geometry = layerWidget->geometry();

        std::cout << QStringLiteral(
                         "[LayerLayoutRow] tree=%1 row=%2 name=\"%3\" viewport=%4 column0=%5 rect=%6,%7 %8x%9 "
                         "legacyResolved=%10 resolved=%11 minHint=%12x%13 sizeHint=%14x%15 "
                         "geometry=%16,%17 %18x%19 itemHint=%20x%21")
                         .arg(treeName)
                         .arg(itemIndex)
                         .arg(layerWidget->debugLayerName())
                         .arg(viewportWidth)
                         .arg(treeWidget->columnWidth(0))
                         .arg(itemRect.x())
                         .arg(itemRect.y())
                         .arg(itemRect.width())
                         .arg(itemRect.height())
                         .arg(legacyResolvedWidth)
                         .arg(resolvedWidth)
                         .arg(minHint.width())
                         .arg(minHint.height())
                         .arg(sizeHint.width())
                         .arg(sizeHint.height())
                         .arg(geometry.x())
                         .arg(geometry.y())
                         .arg(geometry.width())
                         .arg(geometry.height())
                         .arg(itemHint.width())
                         .arg(itemHint.height())
                         .toStdString()
                  << std::endl;
    }
}

SignalLayerWidget::SignalLayerWidget(QWidget *parent) : QFrame(parent) {
    setObjectName("signalLayerWidget");
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFont(sharedLayerRowTextFont());

    auto *rowLayout = new QHBoxLayout(this);
    rowLayout->setContentsMargins(kRowMargins);
    rowLayout->setSpacing(kOuterSpacing);

    visibilityButton = new QToolButton(this);
    visibilityButton->setAutoRaise(true);
    visibilityButton->setCursor(Qt::PointingHandCursor);
    visibilityButton->setToolTip("Toggle visibility");
    visibilityButton->setFixedSize(kVisibilityButtonSize);
    visibilityButton->setIconSize(QSize(16, 16));
    connect(visibilityButton, &QToolButton::clicked, this, [this]() {
        layerVisible = !layerVisible;
        updateVisibilityIcon();
        emit visibilityToggled(layerVisible);
    });

    colorButton = new QPushButton(this);
    colorButton->setFlat(true);
    colorButton->setCursor(Qt::PointingHandCursor);
    colorButton->setToolTip("Change display color");
    colorButton->setFixedSize(kColorButtonSize);
    connect(colorButton, &QPushButton::clicked, this, [this]() {
        if (!usesEdgeStatusColors) {
            emit colorRequested();
        }
    });

    leftZone = new QWidget(this);
    leftZone->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    leftZone->setFixedWidth(leftZoneWidth());
    leftZone->setAutoFillBackground(false);
    leftZone->setStyleSheet("background: transparent; border: 0px;");
    auto *leftLayout = new QHBoxLayout(leftZone);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(kOuterSpacing);
    leftLayout->addWidget(visibilityButton, 0, Qt::AlignVCenter);
    leftLayout->addWidget(colorButton, 0, Qt::AlignVCenter);

    textZone = new QWidget(this);
    textZone->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    textZone->setAutoFillBackground(false);
    textZone->setStyleSheet("background: transparent; border: 0px;");
    auto *textLayout = new QVBoxLayout(textZone);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    nameLabel = new QLabel(textZone);
    nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    nameLabel->setMinimumWidth(0);
    nameLabel->setWordWrap(false);
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nameLabel->setCursor(Qt::PointingHandCursor);
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->installEventFilter(this);
    textLayout->addWidget(nameLabel);

    metadataRow = new QWidget(textZone);
    metadataRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    metadataRow->setAutoFillBackground(false);
    metadataRow->setStyleSheet("background: transparent; border: 0px;");
    auto *metadataLayout = new QHBoxLayout(metadataRow);
    metadataLayout->setContentsMargins(0, 0, 0, 0);
    metadataLayout->setSpacing(kMetadataSpacing);

    contrastButton = new QToolButton(metadataRow);
    contrastButton->setAutoRaise(true);
    contrastButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    contrastButton->setCursor(Qt::PointingHandCursor);
    contrastButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    contrastButton->setToolTip("Adjust contrast");
    contrastButton->setIconSize(QSize(0, 0));
    connect(contrastButton, &QToolButton::clicked, this, [this]() {
        emit contrastRequested(contrastButton);
    });

    opacityButton = new QToolButton(metadataRow);
    opacityButton->setAutoRaise(true);
    opacityButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    opacityButton->setCursor(Qt::PointingHandCursor);
    opacityButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    opacityButton->setToolTip("Adjust opacity");
    opacityButton->setIconSize(QSize(0, 0));
    connect(opacityButton, &QToolButton::clicked, this, [this]() {
        emit opacityRequested(opacityButton);
    });

    metadataLayout->addWidget(contrastButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
    metadataLayout->addStretch(1);
    metadataLayout->addWidget(opacityButton, 0, Qt::AlignRight | Qt::AlignVCenter);
    textLayout->addWidget(metadataRow);

    rowLayout->addWidget(leftZone, 0, Qt::AlignVCenter);
    rowLayout->addWidget(textZone, 1, Qt::AlignVCenter);

    updateChipVisibilityState();
    updateStyle();
    updateVisibilityIcon();
}

void SignalLayerWidget::applyPresentation(const Presentation &presentation) {
    ++presentationUpdateDepth;
    setLayerName(presentation.layerName);
    setLayerColor(presentation.layerColor);
    setUsesCategoricalPalette(presentation.usesCategoricalPalette);
    setUsesEdgeStatusColors(presentation.usesEdgeStatusColors);
    setLayerVisible(presentation.layerVisible);
    setSelected(presentation.selected);
    setContrastText(presentation.contrastText);
    setOpacityText(presentation.opacityText);
    setContrastAvailable(presentation.contrastAvailable);
    setLayerToolTip(presentation.toolTip);
    --presentationUpdateDepth;

    if (presentationUpdateDepth == 0) {
        requestLayoutRefresh();
    }
}

QSize SignalLayerWidget::minimumSizeHint() const {
    const QFontMetrics metrics(font());
    const int textWidth = minimumTextColumnWidth(metrics,
                                                 hasSemanticContrastChip() ? contrastButton->text() : QString(),
                                                 hasSemanticOpacityChip() ? opacityButton->text() : QString());
    const int width = kRowMargins.left() + leftZoneWidth() + kOuterSpacing + textWidth + kRowMargins.right();
    return QSize(width, rowFixedHeightForFont(metrics));
}

QSize SignalLayerWidget::preferredSizeForWidth(int width) const {
    const int resolvedWidth = width > 0 ? width : minimumSizeHint().width();
    return QSize(resolvedWidth, minimumSizeHint().height());
}

QSize SignalLayerWidget::sizeHint() const {
    return preferredSizeForWidth(width());
}

QString SignalLayerWidget::debugLayerName() const {
    return fullLayerName.isEmpty() ? nameLabel->text() : fullLayerName;
}

void SignalLayerWidget::setLayerName(const QString &name) {
    fullLayerName = name;
    updateDisplayedLayerName();
    updateToolTips();
}

void SignalLayerWidget::setLayerColor(const QColor &color) {
    layerColor = color;
    updateColorButtonAppearance();
}

void SignalLayerWidget::setUsesCategoricalPalette(bool usesCategoricalPaletteIn) {
    usesCategoricalPalette = usesCategoricalPaletteIn;
    updateColorButtonAppearance();
}

void SignalLayerWidget::setUsesEdgeStatusColors(bool usesEdgeStatusColorsIn) {
    usesEdgeStatusColors = usesEdgeStatusColorsIn;
    updateColorButtonAppearance();
}

void SignalLayerWidget::setLayerVisible(bool visible) {
    layerVisible = visible;
    updateVisibilityIcon();
}

void SignalLayerWidget::setSelected(bool selectedIn) {
    if (selected == selectedIn) {
        return;
    }

    selected = selectedIn;
    updateStyle();
}

void SignalLayerWidget::setContrastText(const QString &text) {
    contrastButton->setText(text);
    updateChipVisibilityState();
    requestLayoutRefresh();
}

void SignalLayerWidget::setOpacityText(const QString &text) {
    opacityButton->setText(text);
    updateChipVisibilityState();
    requestLayoutRefresh();
}

void SignalLayerWidget::setContrastAvailable(bool available) {
    contrastButton->setVisible(available);
    updateChipVisibilityState();
    requestLayoutRefresh();
}

void SignalLayerWidget::setLayerToolTip(const QString &toolTip) {
    currentLayerToolTip = toolTip;
    updateToolTips();
    if (usesEdgeStatusColors) {
        colorButton->setToolTip("Edge colors are fixed: white, red, green");
    } else if (usesCategoricalPalette) {
        colorButton->setToolTip("Randomize categorical colors");
    } else {
        colorButton->setToolTip("Change display color");
    }
}

bool SignalLayerWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == nameLabel && event != nullptr) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            suppressNextNameRelease = true;
            emit renameRequested();
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease) {
            if (suppressNextNameRelease) {
                suppressNextNameRelease = false;
                return true;
            }
            emit activated();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void SignalLayerWidget::enterEvent(QEnterEvent *event) {
#else
void SignalLayerWidget::enterEvent(QEvent *event) {
#endif
    QFrame::enterEvent(event);
    if (!hovered) {
        hovered = true;
        updateStyle();
    }
}

void SignalLayerWidget::leaveEvent(QEvent *event) {
    QFrame::leaveEvent(event);
    if (hovered) {
        hovered = false;
        updateStyle();
    }
}

void SignalLayerWidget::resizeEvent(QResizeEvent *event) {
    QFrame::resizeEvent(event);
    updateDisplayedLayerName();
}

void SignalLayerWidget::updateDisplayedLayerName() {
    if (nameLabel == nullptr) {
        return;
    }

    const QFontMetrics metrics(nameLabel->font());
    int availableWidth = nameLabel->width();
    if (availableWidth <= 0 && textZone != nullptr) {
        availableWidth = textZone->width();
    }
    if (availableWidth <= 0 && width() > 0) {
        availableWidth = width() - kRowMargins.left() - leftZoneWidth() - kOuterSpacing - kRowMargins.right();
    }

    const QString displayed = fullLayerName.isEmpty()
                                  ? QString()
                                  : abbreviateLayerNameWithMarker(fullLayerName, metrics, availableWidth);
    if (nameLabel->text() != displayed) {
        nameLabel->setText(displayed);
    }
}

void SignalLayerWidget::updateChipVisibilityState() {
    const bool semanticHasContrastChip = hasSemanticContrastChip();
    const bool semanticHasOpacityChip = hasSemanticOpacityChip();

    if (contrastButton != nullptr && contrastButton->isVisible() != semanticHasContrastChip) {
        contrastButton->setVisible(semanticHasContrastChip);
    }
    if (opacityButton != nullptr && opacityButton->isVisible() != semanticHasOpacityChip) {
        opacityButton->setVisible(semanticHasOpacityChip);
    }
    if (metadataRow != nullptr) {
        metadataRow->setVisible(semanticHasContrastChip || semanticHasOpacityChip);
    }
}

void SignalLayerWidget::updateToolTips() {
    QString tooltipText = fullLayerName;
    if (!currentLayerToolTip.isEmpty() && currentLayerToolTip != fullLayerName) {
        tooltipText += QStringLiteral("\n\n") + currentLayerToolTip;
    }
    setToolTip(tooltipText);
    if (nameLabel != nullptr) {
        nameLabel->setToolTip(tooltipText);
    }
}

void SignalLayerWidget::requestLayoutRefresh() {
    if (presentationUpdateDepth > 0) {
        return;
    }

    updateDisplayedLayerName();
    updateGeometry();
    emit sizeHintChanged();
    if (QTreeWidget *treeWidget = hostTreeWidgetFor(this)) {
        requestHostTreeLayoutSync(treeWidget);
    }
}

void SignalLayerWidget::updateColorButtonAppearance() {
    if (usesEdgeStatusColors) {
        colorButton->setIcon(makeEdgeStatusIcon());
        colorButton->setIconSize(QSize(24, 20));
        colorButton->setText(QString());
        colorButton->setCursor(Qt::ArrowCursor);
        colorButton->setToolTip("Edge colors are fixed: white, red, green");
        colorButton->setStyleSheet("QPushButton { background: transparent; border: 0px; padding: 0px; }");
        return;
    }

    if (usesCategoricalPalette) {
        colorButton->setIcon(makeCategoricalHeartIcon());
        colorButton->setIconSize(QSize(28, 24));
        colorButton->setText(QString());
        colorButton->setCursor(Qt::PointingHandCursor);
        colorButton->setToolTip("Randomize categorical colors");
        colorButton->setStyleSheet("QPushButton { background: transparent; border: 0px; padding: 0px; }");
        return;
    }

    colorButton->setIcon(makeSingleColorHeartIcon(layerColor));
    colorButton->setIconSize(QSize(28, 24));
    colorButton->setText(QString());
    colorButton->setCursor(Qt::PointingHandCursor);
    colorButton->setToolTip("Change display color");
    colorButton->setStyleSheet("QPushButton { background: transparent; border: 0px; padding: 0px; }");
}

void SignalLayerWidget::updateStyle() {
    const QColor stableBase = stableLayerCardBaseColor(this);
    const QColor whiteText = QColor("#f8fafc");
    const QColor mutedText = blendColor(whiteText, stableBase, 0.36);
    const QColor idleBorder = QColor("#346792");
    const QColor hoverBorder = QColor("#DCE7F1");
    const QColor selectedBorder = QColor("#4EC7B0");
    const QColor idleSurface = stableBase;
    QColor selectedSurface = blendColor(stableBase, selectedBorder, 0.24);
    selectedSurface.setAlpha(255);
    const QColor background = selected ? selectedSurface : idleSurface;
    const QColor borderColor = hovered ? hoverBorder : (selected ? selectedBorder : idleBorder);

    setStyleSheet(QString("QFrame#signalLayerWidget { background: %1; border: 1px solid %2; border-radius: 6px; }")
                      .arg(background.name(QColor::HexArgb),
                           borderColor.name(QColor::HexArgb)));

    const QFont textFont = sharedLayerRowTextFont();
    setFont(textFont);
    nameLabel->setFont(textFont);
    contrastButton->setFont(textFont);
    opacityButton->setFont(textFont);

    const QFontMetrics metrics(textFont);
    const int rowHeight = rowFixedHeightForFont(metrics);
    setFixedHeight(rowHeight);
    if (leftZone != nullptr) {
        leftZone->setFixedHeight(rowHeight - kRowMargins.top() - kRowMargins.bottom());
    }
    if (textZone != nullptr) {
        textZone->setFixedHeight(rowHeight - kRowMargins.top() - kRowMargins.bottom());
    }
    if (nameLabel != nullptr) {
        nameLabel->setFixedHeight(metrics.height() + kTextRowSlack);
        nameLabel->setStyleSheet(QString("QLabel { background: transparent; border: 0px; color: %1; }")
                                     .arg(whiteText.name(QColor::HexArgb)));
    }
    if (metadataRow != nullptr) {
        metadataRow->setFixedHeight(metrics.height() + kTextRowSlack);
    }

    const QString metadataStyle = QString(
        "QToolButton { background: transparent; border: 0px; padding: 0px; color: %1; }"
        "QToolButton:hover { color: %2; }")
        .arg(mutedText.name(QColor::HexArgb), whiteText.name(QColor::HexArgb));
    contrastButton->setStyleSheet(metadataStyle);
    opacityButton->setStyleSheet(metadataStyle);

    updateDisplayedLayerName();
    updateColorButtonAppearance();
    updateVisibilityIcon();
}

void SignalLayerWidget::updateVisibilityIcon() {
    const QColor base = stableLayerCardBaseColor(this);
    const QColor whiteText = QColor("#f8fafc");
    const QColor iconColor = layerVisible ? whiteText : blendColor(whiteText, base, 0.55);
    visibilityButton->setIcon(makeEyeIcon(iconColor, layerVisible));
}

bool SignalLayerWidget::hasSemanticContrastChip() const {
    return contrastButton != nullptr
           && !contrastButton->isHidden()
           && !contrastButton->text().isEmpty();
}

bool SignalLayerWidget::hasSemanticOpacityChip() const {
    return opacityButton != nullptr
           && !opacityButton->text().isEmpty();
}
