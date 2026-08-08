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

} // namespace mole
