/*
 * Copyright 2013-2014 Christian Loose <christian.loose@hamburg.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "markdowneditor.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QShortcut>
#include <QStyle>
#include <QTextBlock>
#include <QTextStream>

#include <controls/linenumberarea.h>
#include <cutemarkdownhighlighter.h>

#include "markdownmanipulator.h"
#include "snippetcompleter.h"

#include <spellchecker/dictionary.h>
#include "hunspell/spellchecker.h"
using hunspell::SpellChecker;

#include <QScrollBar>

class ScrollBarFix : public QScrollBar {
public:
    ScrollBarFix(Qt::Orientation orient, QWidget *parent=0)
        : QScrollBar(orient, parent) {}

protected:
    void sliderChange(SliderChange change) {
        if (signalsBlocked() && change == QAbstractSlider::SliderValueChange)
            blockSignals(false);
        QScrollBar::sliderChange(change);
    }
};

MarkdownEditor::MarkdownEditor(QWidget *parent) :
    QPlainTextEdit(parent),
    lineNumberArea(new LineNumberArea(this)),
    spellChecker(new SpellChecker()),
    completer(0),
    showHardLinebreaks(false)
{
    highlighter = new CuteMarkdownHighlighter(this->document(), spellChecker);

    QFont font(QStringLiteral("Monospace"), 10);
    font.setStyleHint(QFont::TypeWriter);

    lineNumberArea->setFont(font);
    setFont(font);

    setVerticalScrollBar(new ScrollBarFix(Qt::Vertical, this));

    connect(this, SIGNAL(blockCountChanged(int)),
            this, SLOT(updateLineNumberAreaWidth(int)));
    connect(this, SIGNAL(updateRequest(QRect, int)),
            this, SLOT(updateLineNumberArea(QRect, int)));

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, SIGNAL(customContextMenuRequested(QPoint)),
            this, SLOT(showContextMenu(QPoint)));

    updateLineNumberAreaWidth(0);

    QAction * actionComplete = new QAction(tr("Snippet Complete"), this);
    actionComplete->setObjectName(QStringLiteral("actionComplete"));
    actionComplete->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Space));
    actionComplete->setProperty("defaultshortcut", actionComplete->shortcut());
    connect(actionComplete, SIGNAL(triggered()),
            this, SLOT(performCompletion()));
    this->addAction(actionComplete);
}

MarkdownEditor::~MarkdownEditor()
{
    delete spellChecker;
}

void MarkdownEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(lineNumberArea);

    int selStart = textCursor().selectionStart();
    int selEnd = textCursor().selectionEnd();

    QPalette palette = lineNumberArea->palette();
    palette.setCurrentColorGroup(QPalette::Active);

    painter.fillRect(event->rect(), palette.color(QPalette::Background));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    qreal bottom = top;

    while (block.isValid() && top <= event->rect().bottom()) {
        top = bottom;

        const qreal height = blockBoundingRect(block).height();
        bottom = top + height;

        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(palette.windowText().color());

            bool selected = (
                                (selStart < block.position() + block.length() && selEnd > block.position())
                                || (selStart == selEnd && selStart == block.position())
                            );

            if (selected) {
                painter.save();
                painter.setPen(palette.highlight().color());
            }

            const QString number = QString::number(blockNumber + 1);
            painter.drawText(0, top, lineNumberArea->width() - 4, height, Qt::AlignRight, number);

            if (selected)
                painter.restore();
        }

        block = block.next();
        ++blockNumber;
    }
}

int MarkdownEditor::lineNumberAreaWidth()
{
    int digits = 2;
    int max = qMax(1, blockCount());
    while (max >= 100) {
        max /= 10;
        ++digits;
    }

    QFont font = lineNumberArea->font();
    const QFontMetrics linefmt(font);
    int space = linefmt.horizontalAdvance(QLatin1Char('9')) * digits;

    return space;
}

void MarkdownEditor::resetHighlighting()
{
//    highlighter->reset();
}

void MarkdownEditor::paintEvent(QPaintEvent *e)
{
    QPlainTextEdit::paintEvent(e);

    // draw line end markers if enabled
    if (showHardLinebreaks) {
        drawLineEndMarker(e);
    }

    // draw column ruler
    if (rulerEnabled) {
        drawRuler(e);
    }
}

void MarkdownEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);

    // update line number area
    QRect cr = contentsRect();
    lineNumberArea->setGeometry(QStyle::visualRect(layoutDirection(), cr,
                                QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height())));
}

void MarkdownEditor::keyPressEvent(QKeyEvent *e)
{
    if (completer && completer->isPopupVisible()) {
        // The following keys are forwarded by the completer to the widget
       switch (e->key()) {
       case Qt::Key_Enter:
       case Qt::Key_Return:
       case Qt::Key_Escape:
       case Qt::Key_Tab:
       case Qt::Key_Backtab:
            e->ignore();
            return; // let the completer do default behavior
       default:
           break;
       }
    }

    if (completer)
        completer->hidePopup();

    QPlainTextEdit::keyPressEvent(e);
}

bool MarkdownEditor::canInsertFromMimeData(const QMimeData *source) const
{
    if (isUrlToLocalFile(source)) {
        return true;
    }

    return QPlainTextEdit::canInsertFromMimeData(source);
}

void MarkdownEditor::insertFromMimeData(const QMimeData *source)
{
    if (isUrlToLocalFile(source)) {
        emit loadDroppedFile(source->urls().first().toLocalFile());
    } else {
        QPlainTextEdit::insertFromMimeData(source);
    }
}

void MarkdownEditor::updateLineNumberAreaWidth(int newBlockCount)
{
    Q_UNUSED(newBlockCount)
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void MarkdownEditor::updateLineNumberArea(QRect rect, int dy)
{
    if (dy)
        lineNumberArea->scroll(0, dy);
    else
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void MarkdownEditor::editorFontChanged(const QFont &font)
{
    lineNumberArea->setFont(font);
    setFont(font);
}

void MarkdownEditor::tabWidthChanged(int tabWidth)
{
    QFontMetrics fm(font());
    setTabStopDistance(tabWidth * fm.horizontalAdvance(QLatin1Char(' ')));
}

void MarkdownEditor::rulerEnabledChanged(bool enabled)
{
    rulerEnabled = enabled;
    viewport()->update();
}

void MarkdownEditor::rulerPosChanged(int pos)
{
    rulerPos = pos;
    viewport()->update();
}

void MarkdownEditor::showContextMenu(QPoint pos)
{
    QMenu *contextMenu = createStandardContextMenu();

    QTextCursor cursor = cursorForPosition(pos);
    int cursorPosition = cursor.position();
    cursor.select(QTextCursor::WordUnderCursor);

    // if word under cursor not spelled correctly, add suggestions to context menu
    if (cursor.hasSelection() && !spellChecker->isCorrect(cursor.selectedText())) {
        contextMenu->addSeparator();

        // add new submenu for the suggestions
        QMenu *subMenu = new QMenu(tr("Suggestions"), contextMenu);
        contextMenu->addMenu(subMenu);

        // add action for each suggested replacement
        const QStringList suggestions = spellChecker->suggestions(cursor.selectedText());
        for (const QString &suggestion : suggestions) {
            QAction *action = subMenu->addAction(suggestion);
            action->setData(cursorPosition);
            connect(action, SIGNAL(triggered()),
                    this, SLOT(replaceWithSuggestion()));
        }

        // disable submenu when no suggestions available
        if (suggestions.isEmpty()) {
            subMenu->setEnabled(false);
        }

        QAction *userWordlistAction = contextMenu->addAction(tr("Add to User Dictionary"));
        userWordlistAction->setData(cursor.selectedText());
        connect(userWordlistAction, SIGNAL(triggered()),
                this, SLOT(addWordToUserWordlist()));
    }

    // show context menu
    contextMenu->exec(mapToGlobal(pos));
    delete contextMenu;
}

void MarkdownEditor::replaceWithSuggestion()
{
    QAction *action = qobject_cast<QAction*>(sender());

    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();

    // replace wrong spelled word with suggestion
    cursor.setPosition(action->data().toInt());
    cursor.select(QTextCursor::WordUnderCursor);
    cursor.insertText(action->text());

    cursor.endEditBlock();
}

void MarkdownEditor::performCompletion()
{
    if (!completer) return;

    QRect popupRect = cursorRect();
    popupRect.setLeft(popupRect.left() + lineNumberAreaWidth());

    QStringList words = extractDistinctWordsFromDocument();
    completer->performCompletion(textUnderCursor(), words, popupRect);
}

void MarkdownEditor::insertSnippet(const QString &completionPrefix, const QString &completion, int newCursorPos)
{
    QTextCursor cursor = this->textCursor();

    // select the completion prefix
    cursor.clearSelection();
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, completionPrefix.length());

    int pos = cursor.position();

    // replace completion prefix with snippet
    cursor.insertText(completion);

    // move cursor to requested position
    cursor.setPosition(pos);
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, newCursorPos);

    this->setTextCursor(cursor);
}

void MarkdownEditor::addWordToUserWordlist()
{
    QAction *action = qobject_cast<QAction*>(sender());
    QString word = action->data().toString();
    spellChecker->addToUserWordlist(word);
}

bool MarkdownEditor::isUrlToLocalFile(const QMimeData *source) const
{
    return source->hasUrls() && (source->urls().count() == 1) && source->urls().first().isLocalFile();
}

void MarkdownEditor::loadStyleFromStylesheet(const QString &fileName)
{
    QFile f(fileName);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QTextStream ts(&f);
    QString input = ts.readAll();

    // parse the stylesheet
//    PegMarkdownHighlight::StyleParser parser(input);
//    QVector<PegMarkdownHighlight::HighlightingStyle> styles = parser.highlightingStyles(this->font());

//    // set new style & rehighlight markdown document
//    highlighter->setStyles(styles);
    highlighter->rehighlight();

    // update color palette
//    this->setPalette(parser.editorPalette());
    this->viewport()->setPalette(this->palette());
}

int MarkdownEditor::countWords() const
{
    QString text = toPlainText();

    // empty or only whitespaces?
    if (text.trimmed().isEmpty()) {
        return 0;
    }

    int words = 0;
    bool lastWasWhitespace = false;
    bool firstCharacter = false;

    for (int i = 0; i < text.count(); ++i) {
        if (text.at(i).isSpace()) {
            if (firstCharacter && !lastWasWhitespace) {
                words++;
            }
            lastWasWhitespace = true;
        }
        else
        {
            firstCharacter = true;
            lastWasWhitespace = false;
        }
    }

    if (!lastWasWhitespace && text.count() > 0) {
        words++;
    }

    return words;
}

void MarkdownEditor::setShowSpecialCharacters(bool enabled)
{
    showHardLinebreaks = enabled;

    QTextOption textOption = document()->defaultTextOption();
    QTextOption::Flags optionFlags = textOption.flags();
    if (enabled) {
        optionFlags |= QTextOption::ShowLineAndParagraphSeparators;
        optionFlags |= QTextOption::ShowTabsAndSpaces;
    } else {
        optionFlags &= ~QTextOption::ShowLineAndParagraphSeparators;
        optionFlags &= ~QTextOption::ShowTabsAndSpaces;
    }
    textOption.setFlags(optionFlags);

    document()->setDefaultTextOption(textOption);

    // repaint
    viewport()->update();
}

void MarkdownEditor::setSpellingCheckEnabled(bool enabled)
{
    highlighter->setSpellingCheckEnabled(enabled);

    // rehighlight markdown document
//    highlighter->reset();
    highlighter->rehighlight();
}

void MarkdownEditor::setSpellingDictionary(const Dictionary &dictionary)
{
    spellChecker->loadDictionary(dictionary.filePath());

    // rehighlight markdown document
   // highlighter->reset();
    highlighter->rehighlight();
}

void MarkdownEditor::setSnippetCompleter(SnippetCompleter *completer)
{
    this->completer = completer;

    connect(completer, SIGNAL(snippetSelected(QString,QString, int)),
            this, SLOT(insertSnippet(QString,QString, int)));
}

void MarkdownEditor::setYamlHeaderSupportEnabled(bool enabled)
{
    Q_UNUSED(enabled)
//    highlighter->setYamlHeaderSupportEnabled(enabled);

    // rehighlight markdown document
    //highlighter->reset();
    highlighter->rehighlight();
}

void MarkdownEditor::gotoLine(int line)
{
    QTextCursor cursor(document()->findBlockByNumber(line-1));
    this->setTextCursor(cursor);
}

void MarkdownEditor::drawLineEndMarker(QPaintEvent *e)
{
    QPainter painter(viewport());

    int leftMargin = qRound(fontMetrics().horizontalAdvance(QStringLiteral(" ")) / 2.0);
    int lineEndCharWidth = fontMetrics().horizontalAdvance(QStringLiteral("\u00B6"));
    int fontHeight = fontMetrics().height();

    QTextBlock block = firstVisibleBlock();
    while (block.isValid()) {
        QRectF blockGeometry = blockBoundingGeometry(block).translated(contentOffset());
        if (blockGeometry.top() > e->rect().bottom())
            break;

        if (block.isVisible() && blockGeometry.toRect().intersects(e->rect())) {
            QString text = block.text();
            if (text.endsWith(QLatin1String("  "))) {
                painter.drawText(blockGeometry.left() + fontMetrics().horizontalAdvance(text) + leftMargin,
                                 blockGeometry.top(),
                                 lineEndCharWidth,
                                 fontHeight,
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("\u00B6"));
            }
        }

        block = block.next();
    }
}

void MarkdownEditor::drawRuler(QPaintEvent *e)
{
    const QRect rect = e->rect();
    const QFont font = currentCharFormat().font();

    // calculate vertical offset corresponding given
    // column margin in font metrics
    int verticalOffset = qRound(QFontMetricsF(font).averageCharWidth() * rulerPos)
            + contentOffset().x()
            + document()->documentMargin();

    // draw a ruler with color invert to background color (better readability)
    // and with 50% opacity
    QPainter p(viewport());
    p.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
    p.setPen(QColor(0xff, 0xff, 0xff));
    p.setOpacity(0.5);

    p.drawLine(verticalOffset, rect.top(), verticalOffset, rect.bottom());
}

QString MarkdownEditor::textUnderCursor() const
{
    QTextCursor cursor = this->textCursor();
    QTextDocument *document = this->document();

    // empty text if cursor at start of line
    if (cursor.atBlockStart()) {
        return QString();
    }

    cursor.clearSelection();

    // move left until we find a space or reach the start of line
    while(!document->characterAt(cursor.position()-1).isSpace() && !cursor.atBlockStart()) {
        cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor);
    }


    return cursor.selectedText();
}

bool GreaterThanMinimumWordLength(const QString &word)
{
    static const int MINIMUM_WORD_LENGTH = 3;
    return word.length() > MINIMUM_WORD_LENGTH;
}

QStringList MarkdownEditor::extractDistinctWordsFromDocument() const
{
    QStringList allWords = retrieveAllWordsFromDocument();
    allWords.removeDuplicates();

    QStringList words = filterWordList(allWords, GreaterThanMinimumWordLength);
    words.sort(Qt::CaseInsensitive);

    return words;
}

QStringList MarkdownEditor::retrieveAllWordsFromDocument() const
{
    return toPlainText().split(QRegularExpression("\\W+"), QString::SkipEmptyParts);
}

template <class UnaryPredicate>
QStringList MarkdownEditor::filterWordList(const QStringList &words, UnaryPredicate predicate) const
{
    QStringList filteredWordList;

    for (const QString &word : words) {
        if (predicate(word))
        {
           filteredWordList << word;
        }
    }

    return filteredWordList;
}
