#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>

namespace gofi {

class TuiMarkdownView {
public:
    enum class KeyResult {
        Ignored,
        Consumed,
        Close,
        EditDescription,
        AddComment,
        OpenBrowser
    };

    TuiMarkdownView() = default;

    void setTitle(const QString& title) { m_title = title; }
    QString title() const { return m_title; }

    void setContent(const QString& content) { m_content = content; m_scroll = 0; }
    QString content() const { return m_content; }

    int scroll() const { return m_scroll; }
    void setScroll(int scroll) { m_scroll = scroll; }
    void resetScroll() { m_scroll = 0; }

    KeyResult handleKey(const QByteArray& seq);
    QString render(int bodyRows, int cols);

    static QString formatInlineMarkdown(const QString& text);
    static QStringList renderMarkdownToAnsi(const QString& markdown, int maxWidth);

private:
    QString m_title;
    QString m_content;
    int m_scroll = 0;
};

} // namespace gofi
