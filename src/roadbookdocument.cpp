// SPDX-FileCopyrightText: 2026 Karten contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roadbookdocument.h"

#include <KLocalizedString>

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPageRanges>
#include <QPagedPaintDevice>
#include <QPainter>

using namespace Qt::StringLiterals;

namespace
{
// Measures in millimetres.
constexpr double RowPadding = 2;
constexpr double RowHeight = RoadbookDocument::MapSize + 2 * RowPadding;
constexpr double DistanceWidth = 30;
constexpr double TextGap = 4;
constexpr double SectionHeight = 9;
constexpr double FooterHeight = 7;
constexpr double ColumnGap = 8;
/// From this width of the paper on, there are two columns of instructions.
constexpr double TwoColumnWidth = 250;

const QColor Ink(0x1a, 0x1a, 0x1a);
const QColor Faint(0x66, 0x66, 0x66);
const QColor Rule(0xb8, 0xb8, 0xb8);
const QColor Band(0xec, 0xec, 0xec);

QFont fontOf(double points, QFont::Weight weight = QFont::Normal)
{
    QFont font = QGuiApplication::font();
    font.setPointSizeF(points);
    font.setWeight(weight);
    return font;
}

/// A painter that is spoken to in millimetres. Text is the reason the
/// painter is not simply scaled: a font keeps its spacing only when it is
/// set in the size it is drawn in.
class Sheet
{
public:
    Sheet(QPainter *painter, double pixelsPerMm)
        : m_painter(painter)
        , m_scale(pixelsPerMm)
    {
    }

    QRectF pixels(const QRectF &rect) const
    {
        return {rect.x() * m_scale, rect.y() * m_scale, rect.width() * m_scale, rect.height() * m_scale};
    }

    /// The height text takes up when wrapped to a width.
    double textHeight(const QString &text, const QFont &font, double width) const
    {
        if (text.isEmpty()) {
            return 0;
        }
        const QFontMetricsF metrics(font, m_painter->device());
        return metrics.boundingRect(QRectF(0, 0, width * m_scale, 1e6), Qt::TextWordWrap, text).height() / m_scale;
    }

    /// Draws text into a rectangle and nowhere else.
    void
    text(const QRectF &rect, const QString &text, const QFont &font, const QColor &colour, int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap) const
    {
        m_painter->save();
        m_painter->setClipRect(pixels(rect));
        m_painter->setFont(font);
        m_painter->setPen(colour);
        m_painter->drawText(pixels(rect), flags, text);
        m_painter->restore();
    }

    /// One line, shortened with an ellipsis when it is too long.
    void line(const QRectF &rect, const QString &text, const QFont &font, const QColor &colour, Qt::Alignment alignment = Qt::AlignLeft) const
    {
        const QFontMetricsF metrics(font, m_painter->device());
        this->text(rect,
                   metrics.elidedText(text, Qt::ElideRight, rect.width() * m_scale),
                   font,
                   colour,
                   int(alignment | Qt::AlignVCenter | Qt::TextSingleLine));
    }

    void rule(double x1, double x2, double y, const QColor &colour, double width) const
    {
        m_painter->setPen(QPen(colour, width * m_scale));
        m_painter->drawLine(QPointF(x1 * m_scale, y * m_scale), QPointF(x2 * m_scale, y * m_scale));
    }

    void fill(const QRectF &rect, const QColor &colour) const
    {
        m_painter->fillRect(pixels(rect), colour);
    }

    /// A map drawn in millimetres, in a frame.
    void map(const QRectF &rect, const QSizeF &size, const QPicture &picture) const
    {
        m_painter->save();
        m_painter->setClipRect(pixels(rect));
        m_painter->translate(rect.x() * m_scale, rect.y() * m_scale);
        // Playing a picture back takes its units for pixels of its own
        // resolution and scales them to the device's. Its units are
        // millimetres, though, and it may have to be fitted.
        const double playback = double(m_painter->device()->logicalDpiX()) / picture.logicalDpiX();
        const double fit = rect.width() / size.width();
        m_painter->scale(fit * m_scale / playback, fit * m_scale / playback);
        const_cast<QPicture &>(picture).play(m_painter);
        m_painter->restore();

        m_painter->setPen(QPen(Faint, 0.25 * m_scale, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        m_painter->setBrush(Qt::NoBrush);
        m_painter->drawRect(pixels(rect));
    }

private:
    QPainter *const m_painter;
    const double m_scale;
};

/// Where on which page something goes.
struct Placement {
    int page = 0;
    QRectF rect;
    int section = 0;
    int row = -1; ///< -1 for the heading of the section
    int number = 0; ///< of the instruction, counted through the roadbook
};
}

int RoadbookDocument::paint(QPagedPaintDevice *device) const
{
    QPainter painter(device);
    if (!painter.isActive()) {
        return 0;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    const double scale = device->logicalDpiX() / 25.4;
    const Sheet sheet(&painter, scale);
    const double width = device->width() / scale;
    const double height = device->height() / scale;
    const double bottom = height - FooterHeight;

    // The head of the first page: what the route is, and the map of it.
    const QFont titleFont = fontOf(17, QFont::Bold);
    const QFont subtitleFont = fontOf(10.5);
    const QFont smallFont = fontOf(8.5);
    const double titleHeight = sheet.textHeight(title, titleFont, width);
    const double subtitleHeight = sheet.textHeight(subtitle, subtitleFont, width);
    const double stopsHeight = sheet.textHeight(stops, subtitleFont, width);
    const double legendHeight = sheet.textHeight(legend, smallFont, width);
    double y = titleHeight + 1 + subtitleHeight + (stopsHeight > 0 ? 1.5 + stopsHeight : 0) + 3;
    QRectF overviewRect;
    if (!overviewSize.isEmpty()) {
        // As wide as the paper, unless that leaves too little of the page.
        const QSizeF fitted = overviewSize.scaled(QSizeF(width, height * 0.45), Qt::KeepAspectRatio);
        overviewRect = QRectF(QPointF((width - fitted.width()) / 2, y), fitted);
        y = overviewRect.bottom() + 2;
    }
    const double legendTop = y;
    const double headBottom = legendTop + legendHeight + 3;

    // The instructions, down one column after the other.
    const int columns = width >= TwoColumnWidth ? 2 : 1;
    const double columnWidth = (width - (columns - 1) * ColumnGap) / columns;
    QList<Placement> placements;
    int page = 0;
    int column = 0;
    y = headBottom;
    const auto makeRoom = [&](double needed) {
        const double top = page == 0 ? headBottom : 0;
        // What does not even fit an empty column is put there all the same.
        if (y + needed <= bottom || y <= top) {
            return;
        }
        if (++column == columns) {
            column = 0;
            ++page;
        }
        y = page == 0 ? headBottom : 0;
    };
    int number = 0;
    for (qsizetype s = 0; s < sections.size(); ++s) {
        const Section &section = sections[s];
        if (sections.size() > 1 && !section.rows.isEmpty()) {
            // A heading does not stand alone at the foot of a column.
            makeRoom(SectionHeight + RowHeight);
            placements.append({page, QRectF(column * (columnWidth + ColumnGap), y, columnWidth, SectionHeight), int(s), -1, 0});
            y += SectionHeight;
        }
        for (qsizetype r = 0; r < section.rows.size(); ++r) {
            makeRoom(RowHeight);
            placements.append({page, QRectF(column * (columnWidth + ColumnGap), y, columnWidth, RowHeight), int(s), int(r), ++number});
            y += RowHeight;
        }
    }
    const int pages = page + 1;

    const QPageRanges ranges = device->pageRanges();
    bool first = true;
    for (page = 0; page < pages; ++page) {
        if (!ranges.isEmpty() && !ranges.contains(page + 1)) {
            continue;
        }
        if (!first && !device->newPage()) {
            break;
        }
        first = false;

        if (page == 0) {
            double top = 0;
            sheet.text(QRectF(0, top, width, titleHeight), title, titleFont, Ink);
            top += titleHeight + 1;
            sheet.text(QRectF(0, top, width, subtitleHeight), subtitle, subtitleFont, Ink);
            top += subtitleHeight + 1.5;
            sheet.text(QRectF(0, top, width, stopsHeight), stops, subtitleFont, Faint);
            if (!overviewRect.isEmpty()) {
                sheet.map(overviewRect, overviewSize, overview);
            }
            sheet.text(QRectF(0, legendTop, width, legendHeight), legend, smallFont, Faint);
            sheet.rule(0, width, headBottom, Rule, 0.2);
        }

        for (const Placement &placement : std::as_const(placements)) {
            if (placement.page != page) {
                continue;
            }
            const QRectF &rect = placement.rect;
            const Section &section = sections[placement.section];
            if (placement.row < 0) {
                sheet.fill(rect.adjusted(0, 1.5, 0, -1), Band);
                const QRectF inside = rect.adjusted(2, 1.5, -2, -1);
                const QFont detailFont = fontOf(9.5);
                const double detailWidth = QFontMetricsF(detailFont, device).horizontalAdvance(section.detail) / scale;
                sheet.line(inside, section.detail, detailFont, Faint, Qt::AlignRight);
                sheet.line(inside.adjusted(0, 0, -detailWidth - 3, 0), section.title, fontOf(11, QFont::DemiBold), Ink);
                continue;
            }
            const Row &row = section.rows[placement.row];

            // How far it is: from the start in large figures, from the last instruction in small.
            const double middle = rect.center().y();
            sheet.line(QRectF(rect.left(), rect.top() + RowPadding, DistanceWidth, 4), QString::number(placement.number), fontOf(8.5), Faint);
            sheet.line(QRectF(rect.left(), middle - 6, DistanceWidth - 1, 8), row.total, fontOf(15, QFont::Bold), Ink);
            sheet.line(QRectF(rect.left(), middle + 2.5, DistanceWidth - 1, 5), row.interval, fontOf(9.5), Faint);

            const QRectF mapRect(rect.left() + DistanceWidth, rect.top() + RowPadding, MapSize, MapSize);
            sheet.map(mapRect, QSizeF(MapSize, MapSize), row.map);

            const double textLeft = mapRect.right() + TextGap;
            const double textWidth = std::max(10.0, rect.right() - textLeft);
            const QFont textFont = fontOf(12.5, QFont::DemiBold);
            const QFont followFont = fontOf(9.5);
            const double textHeight = sheet.textHeight(row.text, textFont, textWidth);
            const double followHeight = sheet.textHeight(row.follow, followFont, textWidth);
            // Around the middle of the row as long as it fits, else from the top.
            const double textTop = std::max(mapRect.top(), middle - (textHeight + 1.5 + followHeight) / 2);
            sheet.text(QRectF(textLeft, textTop, textWidth, mapRect.bottom() - textTop), row.text, textFont, Ink);
            sheet.text(QRectF(textLeft, textTop + textHeight + 1.5, textWidth, std::max(0.0, mapRect.bottom() - textTop - textHeight - 1.5)),
                       row.follow,
                       followFont,
                       Faint);

            sheet.rule(rect.left(), rect.right(), rect.bottom(), Rule, 0.15);
        }

        // Who made the map belongs on every page that has one.
        const QRectF footer(0, height - 4.5, width, 4.5);
        const QString pageText = i18nc("@info in the footer of a printed page; %1 is the title of the document", "%1 · Page %2 of %3", title, page + 1, pages);
        sheet.line(footer.adjusted(width * 0.5, 0, 0, 0), pageText, fontOf(7.5), Faint, Qt::AlignRight);
        sheet.line(footer.adjusted(0, 0, -width * 0.5 - 4, 0), attribution, fontOf(7.5), Faint);
    }
    return pages;
}
