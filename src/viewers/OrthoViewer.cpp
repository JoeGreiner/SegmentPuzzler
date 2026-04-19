#include "OrthoViewer.h"
#include "SliceViewerITKSignal.h"
#include "src/qtUtils/TaskRunner.h"
#include <QDebug>
#include <QHash>
#include <QPainter>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include "src/segment_handling/graphBase.h"
#include <QApplication>
#include <QScreen>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <mutex>

#define CHECK_IF_MAIN_THREAD True

namespace {
constexpr int kIndicatorBorderWidth = 4;
constexpr int kIndicatorMargin = kIndicatorBorderWidth;

struct PlaneAccent {
    QColor indicatorColor;
    QColor controlColor;
};

struct InteractionModePresentation {
    QString primaryAction;
    QString secondaryAction;
    QString tertiaryAction;
    QColor color;
    QColor leftButtonColor;
    QColor rightButtonColor;
    QColor middleButtonColor;
};

struct ShortcutHintPresentation {
    QString id;
    QString keyLabel;
    QString actionLabel;
    QString toolTip;
    bool active = false;
};

struct MouseActionPresentation {
    QString actionLabel;
    QColor color;
    int buttonType = -1;
    bool visible = false;
};

InteractionModePresentation createModePresentation(const QString &primaryAction,
                                                  const QColor &color,
                                                  const QString &secondaryAction = QString(),
                                                  const QString &tertiaryAction = QString(),
                                                  const QColor &leftButtonColor = QColor(),
                                                  const QColor &rightButtonColor = QColor(),
                                                  const QColor &middleButtonColor = QColor()) {
    return {primaryAction, secondaryAction, tertiaryAction, color, leftButtonColor, rightButtonColor, middleButtonColor};
}

const PlaneAccent kXYAccent{QColor("#ff4d4d"), QColor("#c96876")};
const PlaneAccent kXZAccent{QColor("#3bd16f"), QColor("#5bb889")};
const PlaneAccent kYZAccent{QColor("#f2d64b"), QColor("#c8aa54")};
const QColor kLeftMouseActionColor("#55c26a");
const QColor kRightMouseActionColor("#ff6b6b");
const QColor kMiddleMouseActionColor("#f8fafc");
constexpr int kShortcutLegendColumnCount = 4;

InteractionModePresentation createSingleActionWithPan(const QString &action, const QColor &color) {
    return createModePresentation(action,
                                  color,
                                  QString(),
                                  "Pan",
                                  kLeftMouseActionColor,
                                  QColor(),
                                  kMiddleMouseActionColor);
}

InteractionModePresentation createDualActionWithPan(const QString &primaryAction,
                                                    const QString &secondaryAction,
                                                    const QColor &color) {
    return createModePresentation(primaryAction,
                                  color,
                                  secondaryAction,
                                  "Pan",
                                  kLeftMouseActionColor,
                                  kRightMouseActionColor,
                                  kMiddleMouseActionColor);
}

ShortcutHintPresentation createShortcutHint(const QString &id,
                                           const QString &keyLabel,
                                           const QString &actionLabel,
                                           const QString &toolTip,
                                           bool active = false) {
    return {id, keyLabel, actionLabel, toolTip, active};
}

QColor mixColors(const QColor &from, const QColor &to, double toAmount) {
    const double clampedAmount = std::clamp(toAmount, 0.0, 1.0);
    const double fromAmount = 1.0 - clampedAmount;
    return QColor(static_cast<int>(std::lround(from.red() * fromAmount + to.red() * clampedAmount)),
                  static_cast<int>(std::lround(from.green() * fromAmount + to.green() * clampedAmount)),
                  static_cast<int>(std::lround(from.blue() * fromAmount + to.blue() * clampedAmount)));
}

QString cssColor(const QColor &color) {
    return color.name(QColor::HexRgb);
}

QString buildSliderStyle(const PlaneAccent &accent) {
    const QColor grooveColor = mixColors(QColor("#1d2024"), accent.controlColor, 0.18);
    const QColor filledColor = mixColors(QColor("#1d2024"), accent.controlColor, 0.42);
    const QColor borderColor = mixColors(accent.controlColor, QColor("#0f1114"), 0.45);
    const QColor handleColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.08);
    const QColor hoverColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.18);
    const QColor pressedColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.28);

    return QString(R"(
QSlider:focus {
    outline: none;
}
QSlider::groove:vertical {
    background: %1;
    border: 1px solid %2;
    border-radius: 5px;
    width: 10px;
    margin: 4px 7px;
}
QSlider::add-page:vertical {
    background: %1;
    border-radius: 4px;
}
QSlider::sub-page:vertical {
    background: %3;
    border-radius: 4px;
}
QSlider::handle:vertical {
    background: %4;
    border: 1px solid %2;
    border-radius: 7px;
    height: 20px;
    margin: -4px -5px;
}
QSlider::handle:vertical:hover {
    background: %5;
}
QSlider::handle:vertical:pressed {
    background: %6;
}
)")
            .arg(cssColor(grooveColor))
            .arg(cssColor(borderColor))
            .arg(cssColor(filledColor))
            .arg(cssColor(handleColor))
            .arg(cssColor(hoverColor))
            .arg(cssColor(pressedColor));
}

QString buildScrollBarStyle(const PlaneAccent &accent) {
    const QColor grooveColor = mixColors(QColor("#1d2024"), accent.controlColor, 0.20);
    const QColor borderColor = mixColors(accent.controlColor, QColor("#0f1114"), 0.45);
    const QColor handleColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.06);
    const QColor hoverColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.16);
    const QColor pressedColor = mixColors(accent.controlColor, QColor("#ffffff"), 0.26);

    return QString(R"(
QScrollBar:horizontal {
    background: %1;
    border: 1px solid %2;
    border-radius: 6px;
    height: 12px;
    margin: 0;
}
QScrollBar:vertical {
    background: %1;
    border: 1px solid %2;
    border-radius: 6px;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: %3;
    border-radius: 5px;
    min-width: 28px;
}
QScrollBar::handle:vertical {
    background: %3;
    border-radius: 5px;
    min-height: 28px;
}
QScrollBar::handle:horizontal:hover,
QScrollBar::handle:vertical:hover {
    background: %4;
}
QScrollBar::handle:horizontal:pressed,
QScrollBar::handle:vertical:pressed {
    background: %5;
}
QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal,
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    background: transparent;
    border: none;
    width: 0px;
    height: 0px;
}
QScrollBar::add-page:horizontal,
QScrollBar::sub-page:horizontal,
QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: transparent;
    border: none;
}
)")
            .arg(cssColor(grooveColor))
            .arg(cssColor(borderColor))
            .arg(cssColor(handleColor))
            .arg(cssColor(hoverColor))
            .arg(cssColor(pressedColor));
}

void applyPlaneControlStyle(const PlaneAccent &accent,
                            QSlider *slider,
                            QScrollBar *topScrollBar,
                            QScrollBar *bottomScrollBar,
                            QScrollBar *leftScrollBar,
                            QScrollBar *rightScrollBar) {
    if (slider != nullptr) {
        slider->setStyleSheet(buildSliderStyle(accent));
    }

    const QString scrollBarStyle = buildScrollBarStyle(accent);
    if (topScrollBar != nullptr) {
        topScrollBar->setStyleSheet(scrollBarStyle);
    }
    if (bottomScrollBar != nullptr) {
        bottomScrollBar->setStyleSheet(scrollBarStyle);
    }
    if (leftScrollBar != nullptr) {
        leftScrollBar->setStyleSheet(scrollBarStyle);
    }
    if (rightScrollBar != nullptr) {
        rightScrollBar->setStyleSheet(scrollBarStyle);
    }
}

InteractionModePresentation currentInteractionModePresentation(const AnnotationSliceViewer *viewer) {
    if (viewer == nullptr) {
        return createModePresentation("", QColor("#7b8ea1"));
    }

    // The badge shows tool-specific left/right actions plus the shared middle-click pan gesture.
    if (viewer->activeTool == SliceViewer::ToolMode::SelectColor) {
        if (viewer->isPaintModeActive() || viewer->isPaintBoundaryModeActive()) {
            return createSingleActionWithPan("Pick Paint Color", QColor("#9d85ff"));
        }
        return createDualActionWithPan("Merge", "Unmerge", QColor("#7b8ea1"));
    }

    if (viewer->isPaintBoundaryModeActive()) {
        return createDualActionWithPan("Merge", "Unmerge", QColor("#5fa8ff"));
    }
    if (viewer->isPaintModeActive()) {
        return createDualActionWithPan("Add", "Erase", QColor("#cf7df2"));
    }
    if (viewer->isROISelectionModeActive()) {
        return createSingleActionWithPan("ROI Selection", QColor("#d8a14d"));
    }

    switch (viewer->activeTool) {
        case SliceViewer::ToolMode::Ctrl:
            return createSingleActionWithPan("Center Views", QColor("#69a7ff"));
        case SliceViewer::ToolMode::Transfer:
            return createSingleActionWithPan("Transfer", QColor("#55c5b8"));
        case SliceViewer::ToolMode::Delete:
            return createSingleActionWithPan("Delete", QColor("#ff6b6b"));
        case SliceViewer::ToolMode::Split:
            return createSingleActionWithPan("Split", QColor("#f6b257"));
        case SliceViewer::ToolMode::Cut:
            return createSingleActionWithPan("Cut", QColor("#ff935c"));
        case SliceViewer::ToolMode::Refine:
            return createSingleActionWithPan("Refine", QColor("#e07cff"));
        case SliceViewer::ToolMode::Fill:
            return createSingleActionWithPan("Fill", QColor("#72c46d"));
        case SliceViewer::ToolMode::Open:
            return createSingleActionWithPan("Open", QColor("#58b8d6"));
        case SliceViewer::ToolMode::Dilate:
            return createSingleActionWithPan("Dilate", QColor("#7ed957"));
        case SliceViewer::ToolMode::Erode:
            return createSingleActionWithPan("Erode", QColor("#4fa3d1"));
        case SliceViewer::ToolMode::Insert:
            return createSingleActionWithPan("Insert", QColor("#e0a35c"));
        case SliceViewer::ToolMode::View3D:
            return createSingleActionWithPan("3D View", QColor("#8ccf5f"));
        case SliceViewer::ToolMode::View3DCut:
            return createSingleActionWithPan("3D Cut", QColor("#ff8e6e"));
        case SliceViewer::ToolMode::None:
        default:
            return createDualActionWithPan("Merge", "Unmerge", QColor("#7b8ea1"));
    }
}

std::vector<ShortcutHintPresentation> currentShortcutHintPresentation(const AnnotationSliceViewer *viewer,
                                                                     const QSet<QString> &flashedShortcutIds,
                                                                     OrthoViewer::ShortcutLegendProfile profile) {
    const auto isFlashed = [&flashedShortcutIds](const QString &id) {
        return flashedShortcutIds.contains(id);
    };
    const auto activeTool = viewer != nullptr ? viewer->activeTool : SliceViewer::ToolMode::None;
    std::vector<ShortcutHintPresentation> hints{
        createShortcutHint("s", "S", "Transfer",
                           "Hold S and click to transfer the working supervoxel under the cursor to the selected segmentation.",
                           activeTool == SliceViewer::ToolMode::Transfer || isFlashed("s")),
        createShortcutHint("d", "D", "Delete",
                           "Hold D and click to delete the segmentation label under the cursor.",
                           activeTool == SliceViewer::ToolMode::Delete || isFlashed("d")),
        createShortcutHint("x", "X", "Split",
                           "Hold X and click to split the working node under the cursor into its initial nodes.",
                           activeTool == SliceViewer::ToolMode::Split || isFlashed("x")),
        createShortcutHint("c", "C", "Cut",
                           "Hold C and click to cut the initial label under the cursor out of the current working supervoxel.",
                           activeTool == SliceViewer::ToolMode::Cut || isFlashed("c")),
        createShortcutHint("p", "P", "Refine",
                           "Hold P and click to refine the segment under the cursor with the selected refinement.",
                           activeTool == SliceViewer::ToolMode::Refine || isFlashed("p")),
        createShortcutHint("f", "F", "Fill",
                           "Hold F and click to fill holes in the segmentation label under the cursor.",
                           activeTool == SliceViewer::ToolMode::Fill || isFlashed("f")),
        createShortcutHint("g", "G", "Open",
                           "Hold G and click to run a morphological opening on the segmentation label under the cursor.",
                           activeTool == SliceViewer::ToolMode::Open || isFlashed("g")),
        createShortcutHint("j", "J", "Dilate",
                           "Hold J and click to dilate the segmentation label under the cursor by one step.",
                           activeTool == SliceViewer::ToolMode::Dilate || isFlashed("j")),
        createShortcutHint("k", "K", "Erode",
                           "Hold K and click to erode the segmentation label under the cursor by one step.",
                           activeTool == SliceViewer::ToolMode::Erode || isFlashed("k")),
        createShortcutHint("h", "H", "Insert",
                           "Hold H and click to insert the clicked segmentation segment into the initial nodes.",
                           activeTool == SliceViewer::ToolMode::Insert || isFlashed("h")),
        createShortcutHint("q", "Q", "Colorpick",
                           "In paint mode, hold Q and click a segment to use its label as the current paint color.",
                           activeTool == SliceViewer::ToolMode::SelectColor || isFlashed("q")),
        createShortcutHint("r", "R", "Cycle",
                           "Press R to randomize the categorical colors of the working segmentation and refinement.",
                           isFlashed("r")),
        createShortcutHint("ctrl", "Ctrl", "Center",
                           "Hold Ctrl and click to center the linked orthogonal views on the clicked point.",
                           activeTool == SliceViewer::ToolMode::Ctrl || isFlashed("ctrl")),
        createShortcutHint("zoom", "+/-", "Zoom",
                           "Press + to zoom in or - to zoom out in all linked viewers.",
                           isFlashed("zoom")),
        createShortcutHint("slice", "↑/↓", "Indexing",
                           "Press the arrow keys to move one slice up or down in the current stack.",
                           isFlashed("slice")),
        createShortcutHint("brush", "0-9", "Brush",
                           "Press 1 through 0 to set the brush size from small to large.",
                           isFlashed("brush")),
        createShortcutHint("u", "U", "Screenshot",
                           "Press U to export a screenshot of the current orthogonal views.",
                           isFlashed("u")),
        createShortcutHint("v", "V", "Video Export",
                           "Press V to export the current view series for video generation.",
                           isFlashed("v")),
        createShortcutHint("f9", "F9", "Go To XYZ",
                           "Press F9 to jump directly to explicit x, y, z coordinates.",
                           isFlashed("f9")),
        createShortcutHint("f10", "F10", "Go To ID",
                           "Press F10 to find a label ID and center the viewers on it.",
                           isFlashed("f10")),
        createShortcutHint("f8", "F8", "Feature Table",
                           "Press F8 to open the segment feature table: shape features for all labels, sortable and color-coded.",
                           isFlashed("f8")),
        createShortcutHint("3dcut", "F7", "3D Cut",
                           "Press F7, then click a working segment to open the cut-enabled 3D view.",
                           activeTool == SliceViewer::ToolMode::View3DCut || isFlashed("3dcut")),
        createShortcutHint("f1", "F1", "Hotkeys",
                           "Press F1 to open the full hotkey reference dialog.",
                           isFlashed("f1")),
        createShortcutHint("e", "E", "Debug",
                           "Press E to export debug graph and image information to files.",
                           isFlashed("e")),
        createShortcutHint("m", "M", "3D View",
                           "Hold M and click to open a 3D surface view of the clicked segment.",
                           activeTool == SliceViewer::ToolMode::View3D || isFlashed("m")),
        createShortcutHint("n", "N", "3D All",
                           "Press N to open a 3D surface view of all segments at once.",
                           isFlashed("n")),
    };

    if (profile != OrthoViewer::ShortcutLegendProfile::Watershed) {
        return hints;
    }

    static const QSet<QString> watershedShortcutIds{
        "q",
        "r",
        "ctrl",
        "zoom",
        "slice",
        "brush",
        "u",
        "v",
        "e",
    };

    std::vector<ShortcutHintPresentation> filteredHints;
    filteredHints.reserve(hints.size());
    for (const auto &hint : hints) {
        if (watershedShortcutIds.contains(hint.id)) {
            filteredHints.push_back(hint);
        }
    }
    return filteredHints;
}

std::vector<MouseActionPresentation> currentMouseActionPresentation(const AnnotationSliceViewer *viewer) {
    const InteractionModePresentation interaction = currentInteractionModePresentation(viewer);
    return {
        {interaction.primaryAction, interaction.leftButtonColor, 0, interaction.leftButtonColor.isValid() && !interaction.primaryAction.isEmpty()},
        {interaction.secondaryAction, interaction.rightButtonColor, 1, interaction.rightButtonColor.isValid() && !interaction.secondaryAction.isEmpty()},
        {interaction.tertiaryAction, interaction.middleButtonColor, 2, interaction.middleButtonColor.isValid() && !interaction.tertiaryAction.isEmpty()},
    };
}
}

class ShortcutHintChip : public QWidget {
public:
    explicit ShortcutHintChip(QWidget *parent = nullptr)
            : QWidget(parent) {
        setAttribute(Qt::WA_Hover, true);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(24);
    }

    void setPresentation(const ShortcutHintPresentation &presentation) {
        keyLabel = presentation.keyLabel;
        actionLabel = presentation.actionLabel;
        active = presentation.active;
        setToolTip(presentation.toolTip);
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor fillColor = active ? QColor("#22384a") : QColor("#171b20");
        const QColor borderColor = active ? QColor("#9bd1ff") : QColor("#2c333b");
        const QColor keyFillColor = active ? QColor("#9bd1ff") : QColor("#232a32");
        const QColor keyTextColor = active ? QColor("#0e1318") : QColor("#e7edf5");
        const QColor textColor = QColor("#f8fafc");

        const QRectF cardRect(0.5, 0.5, width() - 1.0, height() - 1.0);
        painter.setPen(QPen(borderColor, 1.0));
        painter.setBrush(fillColor);
        painter.drawRoundedRect(cardRect, 6.0, 6.0);

        QFont keyFont = painter.font();
        keyFont.setBold(true);
        painter.setFont(keyFont);
        const QFontMetrics keyMetrics(keyFont);
        const int keyWidth = std::max(24, keyMetrics.horizontalAdvance(keyLabel) + 10);
        const QRectF keyRect(5.0, 4.0, static_cast<qreal>(keyWidth), height() - 8.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(keyFillColor);
        painter.drawRoundedRect(keyRect, 4.0, 4.0);
        painter.setPen(keyTextColor);
        painter.drawText(keyRect, Qt::AlignCenter, keyLabel);

        QFont textFont = painter.font();
        textFont.setBold(false);
        painter.setFont(textFont);
        const QFontMetrics textMetrics(textFont);
        const QRectF textRect(keyRect.right() + 6.0, 0.0, width() - keyRect.right() - 10.0, height());
        const QString elidedLabel = textMetrics.elidedText(actionLabel, Qt::ElideRight, static_cast<int>(textRect.width()));
        painter.setPen(textColor);
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elidedLabel);
    }

private:
    QString keyLabel;
    QString actionLabel;
    bool active = false;
};

class MouseActionChip : public QWidget {
public:
    explicit MouseActionChip(QWidget *parent = nullptr)
            : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(26);
        hide();
    }

    void setPresentation(const MouseActionPresentation &presentation) {
        actionLabel = presentation.actionLabel;
        color = presentation.color;
        buttonType = presentation.buttonType;
        setVisible(presentation.visible);
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        if (actionLabel.isEmpty()) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor fillColor("#171b20");
        const QColor borderColor("#2c333b");
        const QColor textColor("#f8fafc");
        const QColor mouseStrokeColor("#d8dde5");

        const QRectF cardRect(0.5, 0.5, width() - 1.0, height() - 1.0);
        painter.setPen(QPen(borderColor, 1.0));
        painter.setBrush(fillColor);
        painter.drawRoundedRect(cardRect, 6.0, 6.0);

        const QRectF mouseRect(6.0, 4.0, 15.0, height() - 8.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        if (buttonType == 0) {
            painter.drawRoundedRect(QRectF(mouseRect.left() + 1.3, mouseRect.top() + 1.3,
                                           mouseRect.width() / 2.0 - 1.5, mouseRect.height() * 0.42), 2.4, 2.4);
        } else if (buttonType == 1) {
            painter.drawRoundedRect(QRectF(mouseRect.center().x() + 0.2, mouseRect.top() + 1.3,
                                           mouseRect.width() / 2.0 - 1.5, mouseRect.height() * 0.42), 2.4, 2.4);
        } else if (buttonType == 2) {
            painter.drawRoundedRect(QRectF(mouseRect.center().x() - 1.6, mouseRect.top() + 1.0,
                                           3.2, mouseRect.height() * 0.48), 1.7, 1.7);
        }

        painter.setPen(QPen(mouseStrokeColor, 1.1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(mouseRect, 5.0, 5.0);
        painter.drawLine(QPointF(mouseRect.center().x(), mouseRect.top() + 1.5),
                         QPointF(mouseRect.center().x(), mouseRect.top() + mouseRect.height() * 0.50));

        QFont labelFont = painter.font();
        labelFont.setBold(true);
        painter.setFont(labelFont);
        painter.setPen(textColor);
        const QRectF labelRect(mouseRect.right() + 6.0, 0.0, width() - mouseRect.right() - 10.0, height());
        painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, actionLabel);
    }

private:
    QString actionLabel;
    QColor color;
    int buttonType = -1;
};

class ShortcutLegendWidget : public QWidget {
public:
    explicit ShortcutLegendWidget(QWidget *parent = nullptr)
            : QWidget(parent) {
        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(6, 4, 6, 4);
        rootLayout->setSpacing(4);

        mouseLayout = new QHBoxLayout();
        mouseLayout->setContentsMargins(0, 0, 0, 0);
        mouseLayout->setSpacing(6);
        for (int index = 0; index < 3; ++index) {
            auto *chip = new MouseActionChip(this);
            mouseChips.push_back(chip);
            mouseLayout->addWidget(chip);
        }
        rootLayout->addLayout(mouseLayout);

        auto *grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(4);
        for (int column = 0; column < kShortcutLegendColumnCount; ++column) {
            grid->setColumnStretch(column, 1);
        }
        gridLayout = grid;
        rootLayout->addLayout(grid);
        rootLayout->addStretch(1);
        setLayout(rootLayout);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setMouseActions(const std::vector<MouseActionPresentation> &actions) {
        for (int index = 0; index < static_cast<int>(mouseChips.size()); ++index) {
            if (index < static_cast<int>(actions.size())) {
                mouseChips[index]->setPresentation(actions[index]);
            } else {
                mouseChips[index]->hide();
            }
        }
    }

    void setHints(const std::vector<ShortcutHintPresentation> &hints) {
        if (gridLayout == nullptr) {
            return;
        }

        QSet<QString> visibleIds;
        for (int index = 0; index < static_cast<int>(hints.size()); ++index) {
            const auto &hint = hints[index];
            ShortcutHintChip *chip = chips.value(hint.id, nullptr);
            if (chip == nullptr) {
                chip = new ShortcutHintChip(this);
                chips.insert(hint.id, chip);
            }
            const int row = index / kShortcutLegendColumnCount;
            const int column = index % kShortcutLegendColumnCount;
            gridLayout->addWidget(chip, row, column);
            chip->setPresentation(hint);
            chip->show();
            visibleIds.insert(hint.id);
        }

        for (auto it = chips.begin(); it != chips.end(); ++it) {
            if (!visibleIds.contains(it.key())) {
                it.value()->hide();
            }
        }

        const int stretchRow = (static_cast<int>(hints.size()) + kShortcutLegendColumnCount - 1) / kShortcutLegendColumnCount;
        gridLayout->setRowStretch(stretchRow, 1);
    }

private:
    QHBoxLayout *mouseLayout = nullptr;
    QGridLayout *gridLayout = nullptr;
    std::vector<MouseActionChip *> mouseChips;
    QHash<QString, ShortcutHintChip *> chips;
};

OrthoViewer::~OrthoViewer() = default;

class VisibleRectOverlay : public QWidget {
public:
    explicit VisibleRectOverlay(const QColor &color, QWidget *parent = nullptr)
            : QWidget(parent), outlineColor(color) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        hide();
    }

    void updateOutline(const QRect &rect) {
        outlineRect = rect;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        if (!outlineRect.isValid() || outlineRect.width() <= 0 || outlineRect.height() <= 0) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(outlineColor);

        const QRect topRect(outlineRect.left(),
                            outlineRect.top(),
                            outlineRect.width(),
                            kIndicatorBorderWidth);
        const QRect bottomRect(outlineRect.left(),
                               outlineRect.bottom() - kIndicatorBorderWidth + 1,
                               outlineRect.width(),
                               kIndicatorBorderWidth);
        const QRect leftRect(outlineRect.left(),
                             outlineRect.top(),
                             kIndicatorBorderWidth,
                             outlineRect.height());
        const QRect rightRect(outlineRect.right() - kIndicatorBorderWidth + 1,
                              outlineRect.top(),
                              kIndicatorBorderWidth,
                              outlineRect.height());

        painter.fillRect(topRect, outlineColor);
        painter.fillRect(bottomRect, outlineColor);
        painter.fillRect(leftRect, outlineColor);
        painter.fillRect(rightRect, outlineColor);

        static QHash<QString, QString> lastPaintLogs;
        const QString logKey = objectName() + "_paint";
        const QString currentLog = QString("[IndicatorPaint %1] overlayRect=%2,%3 %4x%5 outlineRect=%6,%7 %8x%9 "
                                           "top=%10 bottom=%11 left=%12 right=%13")
                .arg(objectName())
                .arg(geometry().x()).arg(geometry().y()).arg(geometry().width()).arg(geometry().height())
                .arg(outlineRect.x()).arg(outlineRect.y()).arg(outlineRect.width()).arg(outlineRect.height())
                .arg(topRect.x() >= 0 && topRect.y() >= 0 && topRect.right() < width() && topRect.bottom() < height())
                .arg(bottomRect.x() >= 0 && bottomRect.y() >= 0 && bottomRect.right() < width() && bottomRect.bottom() < height())
                .arg(leftRect.x() >= 0 && leftRect.y() >= 0 && leftRect.right() < width() && leftRect.bottom() < height())
                .arg(rightRect.x() >= 0 && rightRect.y() >= 0 && rightRect.right() < width() && rightRect.bottom() < height());
        if (lastPaintLogs.value(logKey) != currentLog) {
            qInfo().noquote() << currentLog;
            lastPaintLogs.insert(logKey, currentLog);
        }
    }

private:
    QColor outlineColor;
    QRect outlineRect;
};

namespace {

VisibleRectOverlay *createPlaneIndicator(const QString &name, const QColor &color, QWidget *parent) {
    auto *indicator = new VisibleRectOverlay(color, parent);
    indicator->setObjectName(name);
    indicator->setGeometry(parent->rect());
    indicator->hide();
    return indicator;
}

void configureLayout(QLayout *layout, int spacing) {
    if (layout == nullptr) {
        return;
    }

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
}

} // namespace


OrthoViewer::OrthoViewer(std::shared_ptr<GraphBase> graphBaseIn, TaskRunner *taskRunnerIn, QWidget *parent) : QWidget(parent) {
    initialized = false;
    graphBase = graphBaseIn;
    taskRunner = taskRunnerIn;

    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    splitterLayout = new QVBoxLayout();

    splitterVertical = new QSplitter(Qt::Vertical);
    splitterHorizontalTop = new QLinkedSplitter();
    splitterHorizontalBottom = new QLinkedSplitter();

    scrollAreaZY = new QScrollAreaNoWheel();
    scrollAreaXZ = new QScrollAreaNoWheel();
    scrollAreaXY = new QScrollAreaNoWheel();
    scrollAreaZY->setObjectName("ScrollAreaYZ");
    scrollAreaXZ->setObjectName("ScrollAreaXZ");
    scrollAreaXY->setObjectName("ScrollAreaXY");

    scrollAreaZY->setFocusPolicy(Qt::NoFocus);
    scrollAreaXZ->setFocusPolicy(Qt::NoFocus);
    scrollAreaXY->setFocusPolicy(Qt::NoFocus);

    scrollAreaZY->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaXZ->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaXY->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    scrollAreaZY->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaXZ->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaXY->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollAreaZY->setIndicatorMargins(kIndicatorMargin, kIndicatorMargin, kIndicatorMargin, kIndicatorMargin);
    scrollAreaXZ->setIndicatorMargins(kIndicatorMargin, kIndicatorMargin, kIndicatorMargin, kIndicatorMargin);
    scrollAreaXY->setIndicatorMargins(kIndicatorMargin, kIndicatorMargin, kIndicatorMargin, kIndicatorMargin);

    scrollAreaZY->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    scrollAreaXZ->setAlignment(Qt::AlignRight | Qt::AlignTop);
    scrollAreaXY->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    scrollAreaZY->viewport()->installEventFilter(this);
    scrollAreaXZ->viewport()->installEventFilter(this);
    scrollAreaXY->viewport()->installEventFilter(this);
    zyIndicator = createPlaneIndicator("YZ", kYZAccent.indicatorColor, scrollAreaZY->viewport());
    xzIndicator = createPlaneIndicator("XZ", kXZAccent.indicatorColor, scrollAreaXZ->viewport());
    xyIndicator = createPlaneIndicator("XY", kXYAccent.indicatorColor, scrollAreaXY->viewport());

    shortcutLegendWidget = new ShortcutLegendWidget();

    viewXY = new QWidget();
    viewXZ = new QWidget();
    viewZY = new QWidget();
    viewXY->setObjectName("OrthoPaneXY");
    viewXZ->setObjectName("OrthoPaneXZ");
    viewZY->setObjectName("OrthoPaneYZ");
    viewXY->installEventFilter(this);
    viewXZ->installEventFilter(this);
    viewZY->installEventFilter(this);

    layoutXY = new QGridLayout();
    layoutXZ = new QGridLayout();
    layoutZY = new QGridLayout();
    configureLayout(layoutXY, 0);
    configureLayout(layoutXZ, 0);
    configureLayout(layoutZY, 0);

    sliderXY = new QSlider(Qt::Vertical);
    sliderXZ = new QSlider(Qt::Vertical);
    sliderZY = new QSlider(Qt::Vertical);
    externalTopScrollBarXY = new QScrollBar(Qt::Horizontal);
    externalBottomScrollBarXY = new QScrollBar(Qt::Horizontal);
    externalLeftScrollBarXY = new QScrollBar(Qt::Vertical);
    externalRightScrollBarXY = new QScrollBar(Qt::Vertical);

    externalTopScrollBarXZ = new QScrollBar(Qt::Horizontal);
    externalBottomScrollBarXZ = new QScrollBar(Qt::Horizontal);
    externalLeftScrollBarXZ = new QScrollBar(Qt::Vertical);
    externalRightScrollBarXZ = new QScrollBar(Qt::Vertical);

    externalTopScrollBarZY = new QScrollBar(Qt::Horizontal);
    externalBottomScrollBarZY = new QScrollBar(Qt::Horizontal);
    externalLeftScrollBarZY = new QScrollBar(Qt::Vertical);
    externalRightScrollBarZY = new QScrollBar(Qt::Vertical);

    sliderXY->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    sliderXZ->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    sliderZY->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
    const auto configureExternalScrollBar = [](QScrollBar *scrollBar) {
        if (scrollBar == nullptr) {
            return;
        }
        scrollBar->setFocusPolicy(Qt::NoFocus);
        scrollBar->show();
    };
    configureExternalScrollBar(externalTopScrollBarXY);
    configureExternalScrollBar(externalBottomScrollBarXY);
    configureExternalScrollBar(externalLeftScrollBarXY);
    configureExternalScrollBar(externalRightScrollBarXY);
    configureExternalScrollBar(externalTopScrollBarXZ);
    configureExternalScrollBar(externalBottomScrollBarXZ);
    configureExternalScrollBar(externalLeftScrollBarXZ);
    configureExternalScrollBar(externalRightScrollBarXZ);
    configureExternalScrollBar(externalTopScrollBarZY);
    configureExternalScrollBar(externalBottomScrollBarZY);
    configureExternalScrollBar(externalLeftScrollBarZY);
    configureExternalScrollBar(externalRightScrollBarZY);
    applyPlaneControlStyle(kXYAccent,
                           sliderXY,
                           externalTopScrollBarXY,
                           externalBottomScrollBarXY,
                           externalLeftScrollBarXY,
                           externalRightScrollBarXY);
    applyPlaneControlStyle(kXZAccent,
                           sliderXZ,
                           externalTopScrollBarXZ,
                           externalBottomScrollBarXZ,
                           externalLeftScrollBarXZ,
                           externalRightScrollBarXZ);
    applyPlaneControlStyle(kYZAccent,
                           sliderZY,
                           externalTopScrollBarZY,
                           externalBottomScrollBarZY,
                           externalLeftScrollBarZY,
                           externalRightScrollBarZY);

    layoutXY->addWidget(externalTopScrollBarXY, 0, 1);
    layoutXY->addWidget(externalLeftScrollBarXY, 1, 0);
    layoutXY->addWidget(scrollAreaXY, 1, 1);
    layoutXY->addWidget(externalRightScrollBarXY, 1, 2);
    layoutXY->addWidget(sliderXY, 1, 3);
    layoutXY->addWidget(externalBottomScrollBarXY, 2, 1);
    layoutXY->setColumnStretch(1, 1);
    layoutXY->setRowStretch(1, 1);

    layoutZY->addWidget(externalTopScrollBarZY, 0, 2);
    layoutZY->addWidget(sliderZY, 1, 0);
    layoutZY->addWidget(externalLeftScrollBarZY, 1, 1);
    layoutZY->addWidget(scrollAreaZY, 1, 2);
    layoutZY->addWidget(externalRightScrollBarZY, 1, 3);
    layoutZY->addWidget(externalBottomScrollBarZY, 2, 2);
    layoutZY->setColumnStretch(2, 1);
    layoutZY->setRowStretch(1, 1);

    layoutXZ->addWidget(externalTopScrollBarXZ, 0, 1);
    layoutXZ->addWidget(externalLeftScrollBarXZ, 1, 0);
    layoutXZ->addWidget(scrollAreaXZ, 1, 1);
    layoutXZ->addWidget(externalRightScrollBarXZ, 1, 2);
    layoutXZ->addWidget(sliderXZ, 1, 3);
    layoutXZ->addWidget(externalBottomScrollBarXZ, 2, 1);
    layoutXZ->setColumnStretch(1, 1);
    layoutXZ->setRowStretch(1, 1);

    viewXY->setLayout(layoutXY);
    viewXZ->setLayout(layoutXZ);
    viewZY->setLayout(layoutZY);


    splitterHorizontalTop->addWidget(viewXY);
    splitterHorizontalTop->addWidget(viewZY);
    splitterHorizontalBottom->addWidget(viewXZ);
    splitterHorizontalBottom->addWidget(shortcutLegendWidget);
    splitterVertical->addWidget(splitterHorizontalTop);
    splitterVertical->addWidget(splitterHorizontalBottom);

    unsigned char stretch_factor_left = 2;
    unsigned char stretch_factor_right = 1;

    splitterHorizontalBottom->setStretchFactor(0, stretch_factor_left);
    splitterHorizontalBottom->setStretchFactor(1, stretch_factor_right);

    splitterHorizontalTop->setStretchFactor(0, stretch_factor_left);
    splitterHorizontalTop->setStretchFactor(1, stretch_factor_right);

    splitterVertical->setStretchFactor(0, stretch_factor_left);
    splitterVertical->setStretchFactor(1, stretch_factor_right);


    // link horizontal and vertical scrollbars
    connect(scrollAreaXY->horizontalScrollBar(), &QScrollBar::valueChanged, scrollAreaXZ->horizontalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaXZ->horizontalScrollBar(), &QScrollBar::valueChanged, scrollAreaXY->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalTopScrollBarXY, &QScrollBar::valueChanged, scrollAreaXY->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalBottomScrollBarXY, &QScrollBar::valueChanged, scrollAreaXY->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalTopScrollBarXZ, &QScrollBar::valueChanged, scrollAreaXZ->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalBottomScrollBarXZ, &QScrollBar::valueChanged, scrollAreaXZ->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalTopScrollBarZY, &QScrollBar::valueChanged, scrollAreaZY->horizontalScrollBar(), &QScrollBar::setValue);
    connect(externalBottomScrollBarZY, &QScrollBar::valueChanged, scrollAreaZY->horizontalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaXY->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaXZ->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaZY->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaXY->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });
    connect(scrollAreaXZ->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });
    connect(scrollAreaZY->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });

    connect(scrollAreaXY->verticalScrollBar(), &QScrollBar::valueChanged, scrollAreaZY->verticalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaZY->verticalScrollBar(), &QScrollBar::valueChanged, scrollAreaXY->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalLeftScrollBarXY, &QScrollBar::valueChanged, scrollAreaXY->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalRightScrollBarXY, &QScrollBar::valueChanged, scrollAreaXY->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalLeftScrollBarXZ, &QScrollBar::valueChanged, scrollAreaXZ->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalRightScrollBarXZ, &QScrollBar::valueChanged, scrollAreaXZ->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalLeftScrollBarZY, &QScrollBar::valueChanged, scrollAreaZY->verticalScrollBar(), &QScrollBar::setValue);
    connect(externalRightScrollBarZY, &QScrollBar::valueChanged, scrollAreaZY->verticalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaXY->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaZY->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaXZ->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updatePlaneIndicators(); });
    connect(scrollAreaXY->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });
    connect(scrollAreaZY->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });
    connect(scrollAreaXZ->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updatePlaneIndicators(); });

    // link horizontal splitters, moveSplitterToLinked() blocks signals during execution to prohibit cycles
    connect(splitterHorizontalTop, &QSplitter::splitterMoved, splitterHorizontalBottom, &QLinkedSplitter::moveSplitterToLinked);
    connect(splitterHorizontalBottom, &QSplitter::splitterMoved, splitterHorizontalTop, &QLinkedSplitter::moveSplitterToLinked);
    connect(splitterHorizontalTop, &QSplitter::splitterMoved, this, [this](int, int) { schedulePlaneIndicatorRefresh(); });
    connect(splitterHorizontalBottom, &QSplitter::splitterMoved, this, [this](int, int) { schedulePlaneIndicatorRefresh(); });
    connect(splitterVertical, &QSplitter::splitterMoved, this, [this](int, int) { schedulePlaneIndicatorRefresh(); });

    splitterLayout->addWidget(splitterVertical);
    setLayout(splitterLayout);

    zy = new AnnotationSliceViewer(graphBase, taskRunner, this);
    xz = new AnnotationSliceViewer(graphBase, taskRunner, this);
    xy = new AnnotationSliceViewer(graphBase, taskRunner, this);
    zy->setObjectName("ViewerYZ");
    xz->setObjectName("ViewerXZ");
    xy->setObjectName("ViewerXY");

    viewerList.reserve(3);
    viewerList.push_back(xy);
    viewerList.push_back(xz);
    viewerList.push_back(zy);

    zy->setLinkedViewers(viewerList);
    xy->setLinkedViewers(viewerList);
    xz->setLinkedViewers(viewerList);
    zy->setOrthoViewer(this);
    xy->setOrthoViewer(this);
    xz->setOrthoViewer(this);

    show();
    refreshInteractionModeIndicators();

    splitterHorizontalBottom->moveSplitterExt(300, 1);
}

void OrthoViewer::refreshViewers() {
    for (auto *viewer : viewerList) {
        viewer->recalculateQImages();
    }
}

bool OrthoViewer::isBusy() const {
    return taskRunner != nullptr && taskRunner->isBusy();
}

TaskRunner *OrthoViewer::getTaskRunner() const {
    return taskRunner;
}

void OrthoViewer::refreshZoomLayout() {
    scrollAreaXY->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    scrollAreaZY->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    scrollAreaXZ->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    shortcutLegendWidget->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    viewXY->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    viewZY->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    viewXZ->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    adjustSplittersForCurrentZoom();
    updatePlaneIndicators();
    refreshInteractionModeIndicators();
}

double OrthoViewer::computeFittedZoom() const {
    if (!initialized || xy == nullptr || xz == nullptr || zy == nullptr) {
        return 1.0;
    }
    if (xy->signalList.empty()) {
        return 1.0;
    }
    if (splitterHorizontalTop == nullptr || splitterVertical == nullptr) {
        return 1.0;
    }
    if (viewXY == nullptr || viewXZ == nullptr || viewZY == nullptr) {
        return 1.0;
    }
    if (scrollAreaXY == nullptr || scrollAreaXZ == nullptr || scrollAreaZY == nullptr) {
        return 1.0;
    }
    if (scrollAreaXY->viewport() == nullptr || scrollAreaXZ->viewport() == nullptr || scrollAreaZY->viewport() == nullptr) {
        return 1.0;
    }

    const int dimX = xy->getDimX();
    const int dimY = xy->getDimY();
    const int dimZ = xy->getDimZ();
    if (dimX <= 0 || dimY <= 0 || dimZ <= 0) {
        return 1.0;
    }

    const int availableWidth = splitterHorizontalTop->width() - splitterHorizontalTop->handleWidth();
    const int availableHeight = splitterVertical->height() - splitterVertical->handleWidth();
    if (availableWidth <= 0 || availableHeight <= 0) {
        return 1.0;
    }

    const int leftChromeWidth = std::max(viewXY->width() - scrollAreaXY->viewport()->width(),
                                         viewXZ->width() - scrollAreaXZ->viewport()->width());
    const int rightChromeWidth = viewZY->width() - scrollAreaZY->viewport()->width();
    const int topChromeHeight = std::max(viewXY->height() - scrollAreaXY->viewport()->height(),
                                         viewZY->height() - scrollAreaZY->viewport()->height());
    const int bottomChromeHeight = viewXZ->height() - scrollAreaXZ->viewport()->height();

    const double zoomFromWidth = static_cast<double>(availableWidth - leftChromeWidth - rightChromeWidth) /
                                 static_cast<double>(dimX + dimZ);
    const double zoomFromHeight = static_cast<double>(availableHeight - topChromeHeight - bottomChromeHeight) /
                                  static_cast<double>(dimY + dimZ);
    const double fittedZoom = std::min(zoomFromWidth, zoomFromHeight);
    if (zoomFromWidth <= 0.0 || zoomFromHeight <= 0.0 || fittedZoom <= 0.0) {
        return 1.0;
    }

    static QString lastFittedZoomLog;
    const QString currentLog = QString("[SplitterFittedZoom] dims=%1x%2x%3 available=%4x%5 "
                                       "chrome left=%6 right=%7 top=%8 bottom=%9 "
                                       "zoom width=%10 height=%11 chosen=%12")
            .arg(dimX).arg(dimY).arg(dimZ)
            .arg(availableWidth).arg(availableHeight)
            .arg(leftChromeWidth).arg(rightChromeWidth)
            .arg(topChromeHeight).arg(bottomChromeHeight)
            .arg(zoomFromWidth, 0, 'f', 6)
            .arg(zoomFromHeight, 0, 'f', 6)
            .arg(fittedZoom, 0, 'f', 6);
    if (lastFittedZoomLog != currentLog) {
        qInfo().noquote() << currentLog;
        lastFittedZoomLog = currentLog;
    }

    return fittedZoom;
}

void OrthoViewer::placeSplittersForZoom(double zoom) {
    if (!initialized || xy == nullptr || xz == nullptr || zy == nullptr) {
        return;
    }
    if (xy->signalList.empty()) {
        return;
    }
    if (splitterHorizontalTop == nullptr || splitterHorizontalBottom == nullptr || splitterVertical == nullptr) {
        return;
    }
    if (viewXY == nullptr || viewXZ == nullptr || viewZY == nullptr) {
        return;
    }
    if (scrollAreaXY == nullptr || scrollAreaXZ == nullptr || scrollAreaZY == nullptr) {
        return;
    }
    if (scrollAreaXY->viewport() == nullptr || scrollAreaXZ->viewport() == nullptr || scrollAreaZY->viewport() == nullptr) {
        return;
    }

    const int dimX = xy->getDimX();
    const int dimY = xy->getDimY();
    if (dimX <= 0 || dimY <= 0 || zoom <= 0.0) {
        return;
    }

    const int availableWidth = splitterHorizontalTop->width() - splitterHorizontalTop->handleWidth();
    const int availableHeight = splitterVertical->height() - splitterVertical->handleWidth();
    if (availableWidth <= 0 || availableHeight <= 0) {
        return;
    }

    const int leftChromeWidth = std::max(viewXY->width() - scrollAreaXY->viewport()->width(),
                                         viewXZ->width() - scrollAreaXZ->viewport()->width());
    const int topChromeHeight = std::max(viewXY->height() - scrollAreaXY->viewport()->height(),
                                         viewZY->height() - scrollAreaZY->viewport()->height());

    int leftWidth = leftChromeWidth + static_cast<int>(std::lround(static_cast<double>(dimX) * zoom));
    int topHeight = topChromeHeight + static_cast<int>(std::lround(static_cast<double>(dimY) * zoom));

    leftWidth = std::clamp(leftWidth, 1, std::max(1, availableWidth - 1));
    topHeight = std::clamp(topHeight, 1, std::max(1, availableHeight - 1));

    const int rightWidth = std::max(1, availableWidth - leftWidth);
    const int bottomHeight = std::max(1, availableHeight - topHeight);

    autoAdjustingSplitters = true;
    {
        const QSignalBlocker blockTop(splitterHorizontalTop);
        const QSignalBlocker blockBottom(splitterHorizontalBottom);
        splitterHorizontalTop->setSizes({leftWidth, rightWidth});
        splitterHorizontalBottom->setSizes({leftWidth, rightWidth});
    }
    {
        const QSignalBlocker blockVertical(splitterVertical);
        splitterVertical->setSizes({topHeight, bottomHeight});
    }
    autoAdjustingSplitters = false;

    static QString lastPlaceLog;
    const QString currentLog = QString("[SplitterPlace] zoom=%1 dims=%2x%3 available=%4x%5 "
                                       "chrome left=%6 top=%7 sizes left=%8 right=%9 top=%10 bottom=%11")
            .arg(zoom, 0, 'f', 6)
            .arg(dimX).arg(dimY)
            .arg(availableWidth).arg(availableHeight)
            .arg(leftChromeWidth).arg(topChromeHeight)
            .arg(leftWidth).arg(rightWidth)
            .arg(topHeight).arg(bottomHeight);
    if (lastPlaceLog != currentLog) {
        qInfo().noquote() << currentLog;
        lastPlaceLog = currentLog;
    }
}

void OrthoViewer::reclaimBoundarySlack() {
    if (autoAdjustingSplitters || !initialized || xy == nullptr || xz == nullptr || zy == nullptr) {
        return;
    }
    if (xy->signalList.empty()) {
        return;
    }
    if (splitterHorizontalTop == nullptr || splitterHorizontalBottom == nullptr || splitterVertical == nullptr) {
        return;
    }
    if (scrollAreaXZ == nullptr || scrollAreaZY == nullptr) {
        return;
    }
    if (scrollAreaXZ->viewport() == nullptr || scrollAreaZY->viewport() == nullptr) {
        return;
    }

    const int yzSlack = std::max(0, scrollAreaZY->viewport()->width() - zy->width());
    const int xzSlack = std::max(0, scrollAreaXZ->viewport()->height() - xz->height());
    if (yzSlack == 0 && xzSlack == 0) {
        return;
    }

    const int availableWidth = splitterHorizontalTop->width() - splitterHorizontalTop->handleWidth();
    const int availableHeight = splitterVertical->height() - splitterVertical->handleWidth();
    if (availableWidth <= 1 || availableHeight <= 1) {
        return;
    }

    const QList<int> horizontalSizes = splitterHorizontalTop->sizes();
    const QList<int> verticalSizes = splitterVertical->sizes();
    if (horizontalSizes.size() < 2 || verticalSizes.size() < 2) {
        return;
    }

    const int currentLeftWidth = horizontalSizes.at(0);
    const int currentRightWidth = horizontalSizes.at(1);
    const int currentTopHeight = verticalSizes.at(0);
    const int currentBottomHeight = verticalSizes.at(1);

    const int targetLeftWidth = std::clamp(currentLeftWidth + yzSlack, 1, std::max(1, availableWidth - 1));
    const int targetRightWidth = std::max(1, availableWidth - targetLeftWidth);
    const int targetTopHeight = std::clamp(currentTopHeight + xzSlack, 1, std::max(1, availableHeight - 1));
    const int targetBottomHeight = std::max(1, availableHeight - targetTopHeight);

    const bool adjustHorizontal = yzSlack > 0 && (currentLeftWidth != targetLeftWidth || currentRightWidth != targetRightWidth);
    const bool adjustVertical = xzSlack > 0 && (currentTopHeight != targetTopHeight || currentBottomHeight != targetBottomHeight);
    if (!adjustHorizontal && !adjustVertical) {
        return;
    }

    autoAdjustingSplitters = true;
    if (adjustHorizontal) {
        const QSignalBlocker blockTop(splitterHorizontalTop);
        const QSignalBlocker blockBottom(splitterHorizontalBottom);
        splitterHorizontalTop->setSizes({targetLeftWidth, targetRightWidth});
        splitterHorizontalBottom->setSizes({targetLeftWidth, targetRightWidth});
    }
    if (adjustVertical) {
        const QSignalBlocker blockVertical(splitterVertical);
        splitterVertical->setSizes({targetTopHeight, targetBottomHeight});
    }
    autoAdjustingSplitters = false;

    static QString lastSlackLog;
    const QString currentLog = QString("[SplitterSlack] yzSlack=%1 xzSlack=%2 "
                                       "current left=%3 right=%4 top=%5 bottom=%6 "
                                       "target left=%7 right=%8 top=%9 bottom=%10 "
                                       "viewer XY=%11x%12 viewer YZ=%13x%14 viewer XZ=%15x%16")
            .arg(yzSlack).arg(xzSlack)
            .arg(currentLeftWidth).arg(currentRightWidth).arg(currentTopHeight).arg(currentBottomHeight)
            .arg(targetLeftWidth).arg(targetRightWidth).arg(targetTopHeight).arg(targetBottomHeight)
            .arg(xy->width()).arg(xy->height())
            .arg(zy->width()).arg(zy->height())
            .arg(xz->width()).arg(xz->height());
    if (lastSlackLog != currentLog) {
        qInfo().noquote() << currentLog;
        lastSlackLog = currentLog;
    }
}

void OrthoViewer::adjustSplittersForCurrentZoom() {
    if (autoAdjustingSplitters || !initialized || xy == nullptr) {
        return;
    }
    if (xy->signalList.empty()) {
        return;
    }

    const double currentZoom = xy->zoomFactor;
    const double fittedZoom = computeFittedZoom();
    if (currentZoom <= 0.0 || fittedZoom <= 0.0) {
        return;
    }

    const double threshold = std::max(1.0, fittedZoom);
    const bool proportionalRegime = currentZoom <= threshold + 1e-6;

    static QString lastAdjustLog;
    const QString currentLog = QString("[SplitterAdjust] currentZoom=%1 fittedZoom=%2 threshold=%3 regime=%4")
            .arg(currentZoom, 0, 'f', 6)
            .arg(fittedZoom, 0, 'f', 6)
            .arg(threshold, 0, 'f', 6)
            .arg(proportionalRegime ? "proportional" : "reclaim-slack");
    if (lastAdjustLog != currentLog) {
        qInfo().noquote() << currentLog;
        lastAdjustLog = currentLog;
    }

    if (proportionalRegime) {
        placeSplittersForZoom(currentZoom);
    } else {
        reclaimBoundarySlack();
    }
}


void OrthoViewer::addSignal(itkSignalBase *signal) {
    std::lock_guard<std::mutex> lock(viewerListMutex);
    if (!initialized) { initialize(); }
    zy->addSignal(new SliceViewerITKSignal(signal, zy->getSliceIndex(), 0));
    xz->addSignal(new SliceViewerITKSignal(signal, xz->getSliceIndex(), 1));
    xy->addSignal(new SliceViewerITKSignal(signal, xy->getSliceIndex(), 2));

    refreshZoomLayout();
    sliderXY->setMinimum(0);
    sliderZY->setMinimum(0);
    sliderXZ->setMinimum(0);
    sliderXY->setMaximum(xy->signalList.front()->getDimZ() - 1);
    sliderZY->setMaximum(xy->signalList.front()->getDimX() - 1);
    sliderXZ->setMaximum(xy->signalList.front()->getDimY() - 1);
}

void OrthoViewer::removeSignal(itkSignalBase *signal) {
    std::lock_guard<std::mutex> lock(viewerListMutex);
    zy->removeSignal(signal);
    xz->removeSignal(signal);
    xy->removeSignal(signal);
}

void OrthoViewer::receiveStatusMessage(QString string) {
    emit sendStatusMessage(string);
}

void OrthoViewer::setMorphologyOpeningRadius(int radius) {
    if (xy != nullptr) {
        xy->setOpeningRadius(radius);
    }
    if (xz != nullptr) {
        xz->setOpeningRadius(radius);
    }
    if (zy != nullptr) {
        zy->setOpeningRadius(radius);
    }
}

void OrthoViewer::setMorphologyClosingRadius(int radius) {
    if (xy != nullptr) {
        xy->setClosingRadius(radius);
    }
    if (xz != nullptr) {
        xz->setClosingRadius(radius);
    }
    if (zy != nullptr) {
        zy->setClosingRadius(radius);
    }
}

void OrthoViewer::setMorphologyDilationRadius(int radius) {
    if (xy != nullptr) {
        xy->setDilationRadius(radius);
    }
    if (xz != nullptr) {
        xz->setDilationRadius(radius);
    }
    if (zy != nullptr) {
        zy->setDilationRadius(radius);
    }
}

void OrthoViewer::setMorphologyErosionRadius(int radius) {
    if (xy != nullptr) {
        xy->setErosionRadius(radius);
    }
    if (xz != nullptr) {
        xz->setErosionRadius(radius);
    }
    if (zy != nullptr) {
        zy->setErosionRadius(radius);
    }
}

void OrthoViewer::initialize() {
    initialized = true;
    zy->setSliceAxis(0);
    xz->setSliceAxis(1);
    xy->setSliceAxis(2);
    scrollAreaZY->setWidget(zy);
    scrollAreaXZ->setWidget(xz);
    scrollAreaXY->setWidget(xy);

    connect(sliderXY, &QSlider::valueChanged, xy, &SliceViewer::setSliceIndex);
    connect(sliderZY, &QSlider::valueChanged, zy, &SliceViewer::setSliceIndex);
    connect(sliderXZ, &QSlider::valueChanged, xz, &SliceViewer::setSliceIndex);

    xy->setLinkedSlider(sliderXY);
    zy->setLinkedSlider(sliderZY);
    xz->setLinkedSlider(sliderXZ);

    connect(xy, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
    connect(xz, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
    connect(zy, &SliceViewer::sendStatusMessage, this, &OrthoViewer::receiveStatusMessage);
    updatePlaneIndicators();
    refreshInteractionModeIndicators();
}

void OrthoViewer::setViewToMiddleOfStack() {
#if CHECK_IF_MAIN_THREAD
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
#endif
    xy->setSliceIndex(xy->getDimZ() / 2);
    xz->setSliceIndex(xy->getDimY() / 2);
    zy->setSliceIndex(xy->getDimX() / 2);

    QTimer::singleShot(0, this, [this]() {
        if (!initialized) {
            return;
        }

        const double fittedZoom = computeFittedZoom();
        if (fittedZoom <= 0.0) {
            return;
        }

        const double initialZoom = std::max(1.0, fittedZoom);
        const double currentZoom = xy->zoomFactor;
        if (currentZoom <= 0.0) {
            return;
        }

        static QString lastInitialZoomLog;
        const QString currentLog = QString("[SplitterInitialZoom] fittedZoom=%1 initialZoom=%2 currentZoom=%3")
                .arg(fittedZoom, 0, 'f', 6)
                .arg(initialZoom, 0, 'f', 6)
                .arg(currentZoom, 0, 'f', 6);
        if (lastInitialZoomLog != currentLog) {
            qInfo().noquote() << currentLog;
            lastInitialZoomLog = currentLog;
        }

        if (std::abs(initialZoom - currentZoom) <= 1e-9) {
            adjustSplittersForCurrentZoom();
            updatePlaneIndicators();
            return;
        }

        xy->modifyZoomInAllViewers(initialZoom / currentZoom);
    });
}

bool OrthoViewer::eventFilter(QObject *watched, QEvent *event) {
    if (event != nullptr && event->type() == QEvent::Resize && initialized) {
        const QObject *xyViewport = scrollAreaXY != nullptr ? scrollAreaXY->viewport() : nullptr;
        const QObject *xzViewport = scrollAreaXZ != nullptr ? scrollAreaXZ->viewport() : nullptr;
        const QObject *zyViewport = scrollAreaZY != nullptr ? scrollAreaZY->viewport() : nullptr;
        if (watched == viewXY || watched == viewXZ || watched == viewZY) {
            refreshInteractionModeIndicators();
            schedulePlaneIndicatorRefresh();
        }
        if (watched == xyViewport || watched == xzViewport || watched == zyViewport) {
            onViewportResized();
        }
    }

    return QWidget::eventFilter(watched, event);
}

void OrthoViewer::onViewportResized() {
    if (!initialized || xy == nullptr) {
        updatePlaneIndicators();
        return;
    }
    updatePlaneIndicators();
    schedulePlaneIndicatorRefresh();
}

bool OrthoViewer::hasCollapsedOrthoPane() const {
    if (splitterVertical == nullptr || splitterHorizontalTop == nullptr || splitterHorizontalBottom == nullptr) {
        return false;
    }

    const auto containsCollapsedPane = [](const QList<int> &sizes) {
        return std::any_of(sizes.cbegin(), sizes.cend(), [](int size) { return size <= 0; });
    };

    return containsCollapsedPane(splitterVertical->sizes()) ||
           containsCollapsedPane(splitterHorizontalTop->sizes()) ||
           containsCollapsedPane(splitterHorizontalBottom->sizes());
}

void OrthoViewer::schedulePlaneIndicatorRefresh() {
    if (!initialized) {
        return;
    }

    QTimer::singleShot(0, this, [this]() {
        updatePlaneIndicators();
        if (hasCollapsedOrthoPane()) {
            QTimer::singleShot(0, this, [this]() { updatePlaneIndicators(); });
        }
    });
}

void OrthoViewer::updateExternalScrollBars() {
    const auto syncExternalScrollBar = [](QScrollBar *internalScrollBar, QScrollBar *externalScrollBarA, QScrollBar *externalScrollBarB) {
        if (internalScrollBar == nullptr) {
            return;
        }

        const auto syncOne = [internalScrollBar](QScrollBar *externalScrollBar) {
            if (externalScrollBar == nullptr) {
                return;
            }

            const QSignalBlocker blocker(externalScrollBar);
            externalScrollBar->setRange(internalScrollBar->minimum(), internalScrollBar->maximum());
            externalScrollBar->setPageStep(internalScrollBar->pageStep());
            externalScrollBar->setSingleStep(internalScrollBar->singleStep());
            externalScrollBar->setValue(internalScrollBar->value());
            externalScrollBar->setVisible(true);
            externalScrollBar->setEnabled(true);
        };

        syncOne(externalScrollBarA);
        syncOne(externalScrollBarB);
    };

    syncExternalScrollBar(scrollAreaXY != nullptr ? scrollAreaXY->horizontalScrollBar() : nullptr,
                          externalTopScrollBarXY,
                          externalBottomScrollBarXY);
    syncExternalScrollBar(scrollAreaXY != nullptr ? scrollAreaXY->verticalScrollBar() : nullptr,
                          externalLeftScrollBarXY,
                          externalRightScrollBarXY);
    syncExternalScrollBar(scrollAreaXZ != nullptr ? scrollAreaXZ->horizontalScrollBar() : nullptr,
                          externalTopScrollBarXZ,
                          externalBottomScrollBarXZ);
    syncExternalScrollBar(scrollAreaXZ != nullptr ? scrollAreaXZ->verticalScrollBar() : nullptr,
                          externalLeftScrollBarXZ,
                          externalRightScrollBarXZ);
    syncExternalScrollBar(scrollAreaZY != nullptr ? scrollAreaZY->horizontalScrollBar() : nullptr,
                          externalTopScrollBarZY,
                          externalBottomScrollBarZY);
    syncExternalScrollBar(scrollAreaZY != nullptr ? scrollAreaZY->verticalScrollBar() : nullptr,
                          externalLeftScrollBarZY,
                          externalRightScrollBarZY);
}

void OrthoViewer::updatePlaneIndicators() {
    updateExternalScrollBars();

    const auto updateIndicator = [](
            VisibleRectOverlay *indicator,
            QScrollAreaNoWheel *scrollArea,
            SliceViewer *viewer) {
        static QHash<QString, QString> lastUpdateLogs;
        const QString name = indicator != nullptr ? indicator->objectName() : "unknown";

        if (indicator == nullptr || scrollArea == nullptr || viewer == nullptr || scrollArea->viewport() == nullptr ||
            viewer->signalList.empty()) {
            if (indicator != nullptr) {
                indicator->hide();
            }
            const QString currentLog = QString("[IndicatorUpdate %1] hidden reason=no-signal-or-widget").arg(name);
            if (lastUpdateLogs.value(name) != currentLog) {
                qInfo().noquote() << currentLog;
                lastUpdateLogs.insert(name, currentLog);
            }
            return;
        }

        const QRect viewportRect = scrollArea->viewport()->rect();
        const QRect viewerRectInViewport(viewer->mapTo(scrollArea->viewport(), QPoint(0, 0)), viewer->size());
        QRect visibleRect = viewerRectInViewport.intersected(viewportRect);
        auto *hBar = scrollArea->horizontalScrollBar();
        auto *vBar = scrollArea->verticalScrollBar();
        const QRect viewportGeometry = scrollArea->viewport()->geometry();
        const QRect contentsRect = scrollArea->contentsRect();
        const QRect scrollAreaRect = scrollArea->rect();
        const QMargins viewportMargins = scrollArea->getIndicatorMargins();

        if (visibleRect.width() <= 0 || visibleRect.height() <= 0) {
            indicator->hide();
            const QString currentLog = QString("[IndicatorUpdate %1] hidden reason=empty-visible-rect "
                                               "viewerGeom=%2,%3 %4x%5 viewerRectViewport=%6,%7 %8x%9 viewportRect=%10,%11 %12x%13 "
                                               "scrollAreaRect=%14,%15 %16x%17 contentsRect=%18,%19 %20x%21 "
                                               "viewportMargins=%22,%23,%24,%25")
                    .arg(name)
                    .arg(viewer->geometry().x()).arg(viewer->geometry().y()).arg(viewer->geometry().width()).arg(viewer->geometry().height())
                    .arg(viewerRectInViewport.x()).arg(viewerRectInViewport.y()).arg(viewerRectInViewport.width()).arg(viewerRectInViewport.height())
                    .arg(scrollArea->viewport()->rect().x()).arg(scrollArea->viewport()->rect().y())
                    .arg(scrollArea->viewport()->rect().width()).arg(scrollArea->viewport()->rect().height())
                    .arg(scrollAreaRect.x()).arg(scrollAreaRect.y()).arg(scrollAreaRect.width()).arg(scrollAreaRect.height())
                    .arg(contentsRect.x()).arg(contentsRect.y()).arg(contentsRect.width()).arg(contentsRect.height())
                    .arg(viewportMargins.left()).arg(viewportMargins.top()).arg(viewportMargins.right()).arg(viewportMargins.bottom());
            if (lastUpdateLogs.value(name) != currentLog) {
                qInfo().noquote() << currentLog;
                lastUpdateLogs.insert(name, currentLog);
            }
            return;
        }

        const QRect outlineRect = visibleRect.adjusted(-kIndicatorBorderWidth,
                                                       -kIndicatorBorderWidth,
                                                       kIndicatorBorderWidth,
                                                       kIndicatorBorderWidth)
                                      .intersected(viewportRect);

        indicator->setGeometry(viewportRect);
        indicator->updateOutline(outlineRect);
        indicator->show();
        indicator->raise();

        const QString currentLog = QString("[IndicatorUpdate %1] viewerGeom=%2,%3 %4x%5 viewerRect=%6,%7 %8x%9 "
                                           "viewerRectViewport=%10,%11 %12x%13 viewerSize=%14x%15 currentSlice=%16x%17 zoom=%18 "
                                           "scrollAreaRect=%19,%20 %21x%22 contentsRect=%23,%24 %25x%26 "
                                           "viewportRect=%27,%28 %29x%30 viewportGeom=%31,%32 %33x%34 viewportMargins=%35,%36,%37,%38 "
                                           "visibleRect=%39,%40 %41x%42 outlineRect=%43,%44 %45x%46 overlayGeom=%47,%48 %49x%50 "
                                           "hBar visible=%51 value=%52 range=%53..%54 page=%55 "
                                           "vBar visible=%56 value=%57 range=%58..%59 page=%60")
                    .arg(name)
                    .arg(viewer->geometry().x()).arg(viewer->geometry().y()).arg(viewer->geometry().width()).arg(viewer->geometry().height())
                    .arg(viewer->rect().x()).arg(viewer->rect().y()).arg(viewer->rect().width()).arg(viewer->rect().height())
                    .arg(viewerRectInViewport.x()).arg(viewerRectInViewport.y()).arg(viewerRectInViewport.width()).arg(viewerRectInViewport.height())
                    .arg(viewer->width()).arg(viewer->height())
                    .arg(viewer->getCurrentSliceWidth()).arg(viewer->getCurrentSliceHeight())
                    .arg(viewer->zoomFactor, 0, 'f', 6)
                    .arg(scrollAreaRect.x()).arg(scrollAreaRect.y()).arg(scrollAreaRect.width()).arg(scrollAreaRect.height())
                    .arg(contentsRect.x()).arg(contentsRect.y()).arg(contentsRect.width()).arg(contentsRect.height())
                    .arg(viewportRect.x()).arg(viewportRect.y()).arg(viewportRect.width()).arg(viewportRect.height())
                    .arg(viewportGeometry.x()).arg(viewportGeometry.y()).arg(viewportGeometry.width()).arg(viewportGeometry.height())
                    .arg(viewportMargins.left()).arg(viewportMargins.top()).arg(viewportMargins.right()).arg(viewportMargins.bottom())
                    .arg(visibleRect.x()).arg(visibleRect.y()).arg(visibleRect.width()).arg(visibleRect.height())
                    .arg(outlineRect.x()).arg(outlineRect.y()).arg(outlineRect.width()).arg(outlineRect.height())
                    .arg(indicator->geometry().x()).arg(indicator->geometry().y()).arg(indicator->geometry().width()).arg(indicator->geometry().height())
                    .arg(hBar != nullptr ? hBar->isVisible() : false)
                    .arg(hBar != nullptr ? hBar->value() : -1)
                    .arg(hBar != nullptr ? hBar->minimum() : -1)
                    .arg(hBar != nullptr ? hBar->maximum() : -1)
                    .arg(hBar != nullptr ? hBar->pageStep() : -1)
                    .arg(vBar != nullptr ? vBar->isVisible() : false)
                    .arg(vBar != nullptr ? vBar->value() : -1)
                    .arg(vBar != nullptr ? vBar->minimum() : -1)
                    .arg(vBar != nullptr ? vBar->maximum() : -1)
                    .arg(vBar != nullptr ? vBar->pageStep() : -1);
        if (lastUpdateLogs.value(name) != currentLog) {
            qInfo().noquote() << currentLog;
            lastUpdateLogs.insert(name, currentLog);
        }
    };

    updateIndicator(xyIndicator, scrollAreaXY, xy);
    updateIndicator(xzIndicator, scrollAreaXZ, xz);
    updateIndicator(zyIndicator, scrollAreaZY, zy);
}

void OrthoViewer::refreshInteractionModeIndicators() {
    if (shortcutLegendWidget == nullptr) {
        return;
    }

    if (xy != nullptr && !xy->signalList.empty()) {
        const InteractionModePresentation presentation = currentInteractionModePresentation(xy);
        static QString lastModeLog;
        const QString modeSummary = presentation.secondaryAction.isEmpty()
                ? presentation.primaryAction
                : QString("%1 / %2").arg(presentation.primaryAction, presentation.secondaryAction);
        const QString currentLog = QString("[ToolModeIndicator] mode=%1 color=%2")
                .arg(modeSummary)
                .arg(presentation.color.name(QColor::HexRgb));
        if (lastModeLog != currentLog) {
            qInfo().noquote() << currentLog;
            lastModeLog = currentLog;
        }
    }

    shortcutLegendWidget->setMouseActions(currentMouseActionPresentation(xy));
    shortcutLegendWidget->setHints(currentShortcutHintPresentation(xy, flashedShortcutIds, shortcutLegendProfile));
}

void OrthoViewer::flashShortcutLegendKey(const QString &shortcutId) {
    if (shortcutId.isEmpty()) {
        return;
    }

    flashedShortcutIds.insert(shortcutId);
    shortcutFlashGenerations[shortcutId] = shortcutFlashGenerations.value(shortcutId) + 1;
    const int generation = shortcutFlashGenerations.value(shortcutId);
    refreshInteractionModeIndicators();

    QTimer::singleShot(700, this, [this, shortcutId, generation]() {
        if (shortcutFlashGenerations.value(shortcutId) != generation) {
            return;
        }
        flashedShortcutIds.remove(shortcutId);
        refreshInteractionModeIndicators();
    });
}

void OrthoViewer::setShortcutLegendProfile(ShortcutLegendProfile profile) {
    if (shortcutLegendProfile == profile) {
        return;
    }

    shortcutLegendProfile = profile;
    refreshInteractionModeIndicators();
}

void OrthoViewer::setAnnotationToolMode(SliceViewer::ToolMode toolMode) {
    if (xy != nullptr) {
        xy->activeTool = toolMode;
    }
    if (xz != nullptr) {
        xz->activeTool = toolMode;
    }
    if (zy != nullptr) {
        zy->activeTool = toolMode;
    }
    refreshInteractionModeIndicators();
}

void OrthoViewer::centerViewportsToXYZImageSpace(int x, int y, int z) {
    centerViewportsToXYViewportSpace(scrollAreaXY,
                                     static_cast<double>(x),
                                     static_cast<double>(y),
                                     xy->zoomFactor);
    centerViewportsToXYViewportSpace(scrollAreaXZ,
                                     static_cast<double>(x),
                                     static_cast<double>(z),
                                     xz->zoomFactor);
    centerViewportsToXYViewportSpace(scrollAreaZY,
                                     static_cast<double>(z),
                                     static_cast<double>(y),
                                     zy->zoomFactor);
}


void OrthoViewer::centerViewportsToXYViewportSpace(QScrollArea* scrollArea,
                                                   double xWanted,
                                                   double yWanted,
                                                   double zoomFactor)
{
    if (!scrollArea)
        return;

    double centerXWanted = xWanted * zoomFactor;
    double centerYWanted = yWanted * zoomFactor;

    QRect visibleRect = scrollArea->viewport()->rect();

    QScrollBar* hBar = scrollArea->horizontalScrollBar();
    QScrollBar* vBar = scrollArea->verticalScrollBar();
    if (!hBar || !vBar)
        return;

    int leftInView   = hBar->value();
    int rightInView  = hBar->value() + visibleRect.width();
    int topInView    = vBar->value();
    int bottomInView = vBar->value() + visibleRect.height();

    bool xIsVisible = (centerXWanted >= leftInView && centerXWanted <= rightInView);
    bool yIsVisible = (centerYWanted >= topInView  && centerYWanted <= bottomInView);

    // Already visible, no scrolling needed.
    if (xIsVisible && yIsVisible) {
        return;
    }

    int desiredHScroll = static_cast<int>(centerXWanted - visibleRect.width() / 2.0);
    int desiredVScroll = static_cast<int>(centerYWanted - visibleRect.height() / 2.0);

    desiredHScroll = std::max(desiredHScroll, hBar->minimum());
    desiredHScroll = std::min(desiredHScroll, hBar->maximum());
    desiredVScroll = std::max(desiredVScroll, vBar->minimum());
    desiredVScroll = std::min(desiredVScroll, vBar->maximum());

    hBar->setValue(desiredHScroll);
    vBar->setValue(desiredVScroll);
}
