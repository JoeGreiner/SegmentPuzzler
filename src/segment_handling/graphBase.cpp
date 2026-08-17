#include "graphBase.h"

#include <QCoreApplication>
#include <QThread>

#include "src/utils/AppLogger.h"

namespace {

bool isGuiThread() {
    return QCoreApplication::instance() == nullptr ||
        QThread::currentThread() == QCoreApplication::instance()->thread();
}

} // namespace

bool GraphBase::rebuildEdgeColorPresentation() {
    if (pEdgesInitialSegmentsITKSignal == nullptr) {
        return false;
    }

    if (!isGuiThread()) {
        SP_LOG_ERROR(
            "viewer.render",
            QStringLiteral("Refusing to rebuild edge colors outside the GUI thread"));
        return false;
    }

    pEdgesInitialSegmentsITKSignal->rebuildEdgeColorTable(
        edgeStatus,
        colorLookUpEdgesStatus);
    return true;
}

bool GraphBase::updateEdgeColorPresentation(const std::set<MappedEdgeIdType> &changedIds) {
    if (pEdgesInitialSegmentsITKSignal == nullptr) {
        return false;
    }

    if (!isGuiThread()) {
        SP_LOG_ERROR(
            "viewer.render",
            QStringLiteral("Refusing to update edge colors outside the GUI thread"));
        return false;
    }

    if (pEdgesInitialSegmentsITKSignal->updateEdgeColorTable(
            changedIds,
            edgeStatus,
            colorLookUpEdgesStatus)) {
        return true;
    }

    pEdgesInitialSegmentsITKSignal->rebuildEdgeColorTable(
        edgeStatus,
        colorLookUpEdgesStatus);
    return true;
}
