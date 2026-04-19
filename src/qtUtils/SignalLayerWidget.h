#ifndef SEGMENTPUZZLER_SIGNALLAYERWIDGET_H
#define SEGMENTPUZZLER_SIGNALLAYERWIDGET_H

#include <QColor>
#include <QFrame>
#include <QString>

class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QWidget;

class SignalLayerWidget : public QFrame {
Q_OBJECT
public:
    struct Presentation {
        QString layerName;
        QColor layerColor = Qt::white;
        QString contrastText;
        QString opacityText;
        QString toolTip;
        bool usesCategoricalPalette = false;
        bool usesEdgeStatusColors = false;
        bool layerVisible = true;
        bool selected = false;
        bool contrastAvailable = true;
    };

    explicit SignalLayerWidget(QWidget *parent = nullptr);

    static void configureHostTree(QTreeWidget *treeWidget);
    static void requestHostTreeLayoutSync(QTreeWidget *treeWidget);
    static void syncHostTreeLayout(QTreeWidget *treeWidget);

    void applyPresentation(const Presentation &presentation);
    QSize minimumSizeHint() const override;
    QSize preferredSizeForWidth(int width) const;
    QSize sizeHint() const override;
    QString debugLayerName() const;

    void setLayerName(const QString &name);
    void setLayerColor(const QColor &color);
    void setUsesCategoricalPalette(bool usesCategoricalPaletteIn);
    void setUsesEdgeStatusColors(bool usesEdgeStatusColorsIn);
    void setLayerVisible(bool visible);
    void setSelected(bool selected);
    void setContrastText(const QString &text);
    void setOpacityText(const QString &text);
    void setContrastAvailable(bool available);
    void setLayerToolTip(const QString &toolTip);

signals:
    void activated();
    void renameRequested();
    void sizeHintChanged();
    void visibilityToggled(bool visible);
    void colorRequested();
    void contrastRequested(QWidget *anchor);
    void opacityRequested(QWidget *anchor);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateDisplayedLayerName();
    void updateChipVisibilityState();
    void updateToolTips();
    void requestLayoutRefresh();
    void updateColorButtonAppearance();
    void updateStyle();
    void updateVisibilityIcon();
    bool hasSemanticContrastChip() const;
    bool hasSemanticOpacityChip() const;

    QWidget *leftZone = nullptr;
    QWidget *textZone = nullptr;
    QWidget *metadataRow = nullptr;
    QToolButton *visibilityButton = nullptr;
    QPushButton *colorButton = nullptr;
    QLabel *nameLabel = nullptr;
    QToolButton *contrastButton = nullptr;
    QToolButton *opacityButton = nullptr;

    QString fullLayerName;
    QString currentLayerToolTip;
    QColor layerColor = Qt::white;
    bool usesCategoricalPalette = false;
    bool usesEdgeStatusColors = false;
    bool layerVisible = true;
    bool selected = false;
    bool hovered = false;
    bool suppressNextNameRelease = false;
    int presentationUpdateDepth = 0;
};

#endif // SEGMENTPUZZLER_SIGNALLAYERWIDGET_H
