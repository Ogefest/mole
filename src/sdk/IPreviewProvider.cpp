#include "sdk/IPreviewProvider.h"

namespace mole {

PreviewController::PreviewController(QObject* parent)
    : QObject(parent)
{
}

PreviewController::~PreviewController() = default;

void PreviewController::setLoading(bool loading)
{
    if (m_loading == loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void PreviewController::setErrorText(const QString& text)
{
    if (m_errorText == text)
        return;
    m_errorText = text;
    emit errorTextChanged();
}

void PreviewController::decline(const QString& reason)
{
    // Set as well as emitted. Whoever is listening is expected to step down the
    // ladder, but nothing here can require it -- a viewer built by a test, or by
    // a host that has not implemented the fallback, still has to say what
    // happened somewhere a person will see it.
    setLoading(false);
    setErrorText(reason);
    emit declined(reason);
}

} // namespace mole
