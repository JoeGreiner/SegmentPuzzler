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

inline QString makeUniqueSignalName(const std::vector<itkSignalBase *> &signalList, const QString &requestedName) {
    if (requestedName.isEmpty() || !signalNameExists(signalList, requestedName)) {
        return requestedName;
    }

    const QString stem = signalNameStem(requestedName);
    for (int suffix = 1;; ++suffix) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(stem).arg(suffix);
        if (!signalNameExists(signalList, candidate)) {
            return candidate;
        }
    }
}

} // namespace signal_name_utils

#endif // SEGMENTPUZZLER_SIGNALNAMEUTILS_H
