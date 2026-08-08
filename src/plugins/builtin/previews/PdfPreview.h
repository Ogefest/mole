#pragma once

#include "sdk/IPreviewProvider.h"

#include <QTemporaryDir>

#include <memory>

#ifdef MOLE_HAVE_QTPDF
#include <QPdfDocument>
#endif

namespace mole {

class LocalCopyProvider;

#ifdef MOLE_HAVE_QTPDF

/// A PDF as pages, rendered one at a time.
///
/// The document is opened read-only -- previewing something is not a licence to
/// modify it -- and a page is rendered only when the view asks for it, because a
/// six-hundred-page scan held as images would be the worst possible way to look at
/// the first page of it. See docs/adr/0004-pdf-previews.md.
class PdfPreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(int pageCount READ pageCount NOTIFY documentChanged)
    /// The page the view is looking at, 0-based, for the position strip.
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY currentPageChanged)
    Q_PROPERTY(QString title READ title NOTIFY documentChanged)

public:
    explicit PdfPreviewController(PluginServices services, QObject* parent = nullptr);
    ~PdfPreviewController() override;

    int pageCount() const;
    int currentPage() const { return m_currentPage; }
    void setCurrentPage(int page);
    QString positionText() const;
    QString title() const;

    /// A `file:` url for `page` rendered `width` pixels wide, or an empty string
    /// when it cannot be rendered. Renders on first ask and reuses the file
    /// afterwards, so scrolling back is free.
    Q_INVOKABLE QString pageImage(int page, int width);
    /// The page's aspect, so a delegate can reserve the right height before the
    /// image exists and the list does not jump as pages arrive.
    Q_INVOKABLE double pageAspect(int page) const;

    void load(const FileEntry& entry) override;

signals:
    void documentChanged();
    void currentPageChanged();

private:
    void openLocalFile(const QString& path);

    PluginServices m_services;
    std::unique_ptr<LocalCopyProvider> m_copy;
    std::unique_ptr<QPdfDocument> m_document;
    /// Where rendered pages go. One directory per preview, gone when it closes.
    std::unique_ptr<QTemporaryDir> m_scratch;
    int m_currentPage = 0;
    QString m_fileName;
};

#endif // MOLE_HAVE_QTPDF

/// Claims `.pdf` -- but only in a build that can actually render one.
///
/// Without `Qt6::Pdf` this provider still exists and still refuses every file, so
/// a PDF falls through to the information viewer rather than opening an empty
/// frame. The image provider sets the same precedent by claiming only what its
/// build can decode.
class PdfPreviewProvider final : public IPreviewProvider
{
public:
    explicit PdfPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.pdf"); }
    QString displayName() const override { return QStringLiteral("Document"); }
    /// Above the text viewer, which would otherwise treat a PDF as bytes.
    int priority() const override { return 70; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    /// True when this build can render a PDF at all.
    static bool isAvailable();

private:
    PluginServices m_services;
};

} // namespace mole
