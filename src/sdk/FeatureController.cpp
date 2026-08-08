#include "sdk/FeatureController.h"

namespace mole {

FeatureController::FeatureController(QString title, QObject* parent)
    : QObject(parent)
    , m_title(std::move(title))
{
}

FeatureController::~FeatureController() = default;

void FeatureController::setTitle(const QString& title)
{
    if (m_title == title)
        return;
    m_title = title;
    emit titleChanged();
}

void FeatureController::setSubtitle(const QString& subtitle)
{
    if (m_subtitle == subtitle)
        return;
    m_subtitle = subtitle;
    emit subtitleChanged();
}

void FeatureController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

} // namespace mole
