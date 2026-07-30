#ifndef SEGMENTPUZZLER_SIGNALNAMEUTILS_H
#define SEGMENTPUZZLER_SIGNALNAMEUTILS_H

#include <QRegularExpression>
#include <QString>
#include <vector>

#include "src/viewers/itkSignalBase.h"

namespace signal_name_utils {

inline QString signalNameStem(const QString &name) {
    static const QRegularExpression suffixPattern(QStringLiteral(R"(^(.*) \((\d+)\)$)"));
    const QRegularExpressionMatch match = suffixPattern.match(name);
    return match.hasMatch() ? match.captured(1) : name;
}

inline bool signalNameExists(const std::vector<itkSignalBase *> &signalList, const QString &name) {
    for (const auto *signal : signalList) {
        if (signal != nullptr && signal->name == name) {
            return true;
        }
    }
    return false;
}

inline QString makeUniqueSignalName(const std::vector<itkSignalBase *> &signalList,
                                    const QString &requestedName,
                                    const std::vector<QString> &reservedSuffixes = {}) {
    const auto nameExists = [&](const QString &name) {
        if (signalNameExists(signalList, name)) {
            return true;
        }
        for (const QString &suffix : reservedSuffixes) {
            if (signalNameExists(signalList, name + suffix)) {
                return true;
            }
        }
        return false;
    };

    if (requestedName.isEmpty() || !nameExists(requestedName)) {
        return requestedName;
    }

    const QString stem = signalNameStem(requestedName);
    for (int suffix = 1;; ++suffix) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(stem).arg(suffix);
        if (!nameExists(candidate)) {
            return candidate;
        }
    }
}

} // namespace signal_name_utils

#endif // SEGMENTPUZZLER_SIGNALNAMEUTILS_H
