#ifndef SEGMENTPUZZLER_EXPORTPATHUTILS_H
#define SEGMENTPUZZLER_EXPORTPATHUTILS_H

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <initializer_list>

namespace export_path_utils {

inline QString sanitizedFileNameStem(QString stem) {
    stem = stem.trimmed();
    const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (int index = 0; index < stem.size(); ++index) {
        const QChar character = stem.at(index);
        if (character.unicode() < 0x20 || invalidCharacters.contains(character)) {
            stem[index] = QLatin1Char('_');
        }
    }
    while (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' '))) {
        stem.chop(1);
    }
    return stem;
}

inline QString sourceStem(const QString &sourcePath) {
    const QFileInfo sourceInfo(sourcePath);
    const QString fileName = sourceInfo.fileName();
    const QString niiGzSuffix = QStringLiteral(".nii.gz");
    if (fileName.endsWith(niiGzSuffix, Qt::CaseInsensitive)) {
        return fileName.left(fileName.size() - niiGzSuffix.size());
    }
    return sourceInfo.completeBaseName();
}

inline QString directoryFromStoredPath(const QString &storedPath) {
    if (storedPath.isEmpty()) {
        return {};
    }

    const QFileInfo storedInfo(storedPath);
    return storedInfo.exists() && storedInfo.isDir()
               ? storedInfo.absoluteFilePath()
               : storedInfo.absolutePath();
}

inline QString suggestedExportPath(const QString &storedDefaultPath,
                                   std::initializer_list<QString> sourcePathsByPriority,
                                   const QString &fallbackStem,
                                   const QString &defaultStem,
                                   const QString &fileNameEnding) {
    QString sourcePath;
    for (const QString &candidate : sourcePathsByPriority) {
        if (!candidate.isEmpty()) {
            sourcePath = candidate;
            break;
        }
    }

    QString directoryPath = sourcePath.isEmpty()
                                ? QString()
                                : QFileInfo(sourcePath).absolutePath();
    if (directoryPath.isEmpty()) {
        directoryPath = directoryFromStoredPath(storedDefaultPath);
    }

    QString stem = sourcePath.isEmpty() ? QString() : sourceStem(sourcePath);
    if (stem.trimmed().isEmpty()) {
        stem = fallbackStem;
    }
    stem = sanitizedFileNameStem(stem);
    if (stem.isEmpty()) {
        stem = sanitizedFileNameStem(defaultStem);
    }

    const QString fileName = stem + fileNameEnding;
    return directoryPath.isEmpty() ? fileName : QDir(directoryPath).filePath(fileName);
}

} // namespace export_path_utils

#endif // SEGMENTPUZZLER_EXPORTPATHUTILS_H
