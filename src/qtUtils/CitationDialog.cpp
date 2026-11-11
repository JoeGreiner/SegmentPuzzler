#include "CitationDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

CitationDialog::CitationDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Cite SegmentPuzzler"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(680, 400);

    auto *layout = new QVBoxLayout(this);
    auto *introduction = new QLabel(tr(
        "If you use SegmentPuzzler in your research, please consider citing "
        "the following publication."), this);
    introduction->setWordWrap(true);
    layout->addWidget(introduction);

    auto *formatRow = new QHBoxLayout;
    auto *formatLabel = new QLabel(tr("&Format:"), this);
    auto *format = new QComboBox(this);
    format->addItem(QStringLiteral("APA"), QStringLiteral(":/citations/apa.txt"));
    format->addItem(QStringLiteral("BibTeX"), QStringLiteral(":/citations/bibtex.bib"));
    formatLabel->setBuddy(format);
    formatRow->addWidget(formatLabel);
    formatRow->addWidget(format);
    formatRow->addStretch();
    layout->addLayout(formatRow);

    auto *citation = new QPlainTextEdit(this);
    citation->setReadOnly(true);
    citation->setAccessibleName(tr("Citation"));
    layout->addWidget(citation);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *copy = buttons->addButton(tr("Copy citation"), QDialogButtonBox::ActionRole);
    auto *publication = buttons->addButton(tr("Open publication"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(copy, &QPushButton::clicked, this, [citation]() {
        QApplication::clipboard()->setText(citation->toPlainText());
    });
    connect(publication, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://doi.org/10.1113/JP288557")));
    });
    layout->addWidget(buttons);

    const auto updateCitation = [this, format, citation, copy]() {
        QFile file(format->currentData().toString());
        const bool loaded = file.open(QIODevice::ReadOnly);
        citation->setFont(format->currentIndex() == 1
            ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : font());
        citation->setPlainText(loaded ? QString::fromUtf8(file.readAll()).trimmed()
                                     : tr("Could not load the citation text."));
        copy->setEnabled(loaded);
    };
    connect(format, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateCitation);
    updateCitation();
}
