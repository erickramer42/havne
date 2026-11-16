// File/layer: app/framelistmodel.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge); QML: QmlTest.log

#pragma once

#include <QObject>
#include <QString>
#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>
#include <QByteArray>
#include <QtGlobal>

#include <deque>
#include <set>
#include <vector>

#include "framefilter.h"
#include "ecuio/core.h"
#include "ecuio/signals.h"

// The Log page's frames, newest first by default. The last MaxFrames
// in a fixed ring of plain rows: each frame overwrites the oldest once
// it's full, so a frame costs the same at the cap as below it, and the
// hex is formatted only for the rows a view asks for. Row order is
// arrival, never the frame's timestamp, which comes from the producer's
// clock (on J2534, the adapter's deltas after the first frame).
// Rows can be selected for copying: the selection holds frames, not row
// numbers, so it stays on them as rows arrive, the order flips or the
// ring wraps, and a frame leaving the ring leaves the selection.
// A filter (FrameFilter) shows only the frames it passes: the rows are
// then those frames' arrival numbers, kept as frames come and go, so a
// frame costs the same filtered as not; count is the rows shown and
// heldCount the frames the ring holds.
class FrameListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int heldCount READ heldCount NOTIFY countChanged)
    Q_PROPERTY(int filterTerms READ filterTerms NOTIFY filterChanged)
    Q_PROPERTY(bool newestFirst READ newestFirst WRITE setNewestFirst NOTIFY newestFirstChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)

public:
    explicit FrameListModel(QObject* parent = nullptr);
    static constexpr int MaxFrames = 10000;

    enum FrameRoles {
        TimestampRole = Qt::UserRole + 1,
        DlcRole,
        ExtendedRole,
        PayloadRole,
        ArbitrationIdRole,  /* appended, so existing role numbers are unchanged */
        SelectedRole,
        FlagsRole,          /* the frame's ECUIO_FRAME_FLAG_* */
        FlagsTextRole       /* flagWords(): "FD reassembled", "" for none */
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // len is the frame's data_length, already clamped to its buffer
    void appendFrame(const ecuio_frame_t& frame, uint16_t len);
    void clear();
    // The rows shown: every frame held, or those the filter passes
    int count() const;
    int heldCount() const { return m_count; }
    bool newestFirst() const { return m_newestFirst; }
    void setNewestFirst(bool newestFirst);

    // The filter (framefilter.h's syntax): the ID and data fields' text,
    // FrameFilter::Direction, FrameFilter::Flag, and the length and time
    // fields' text. A field that doesn't parse keeps its part of the
    // filter in force as it was; the answer is { ids, data, lengths,
    // times }, each "" or the words for its field. A change shows the
    // rows anew and lets the selection go
    Q_INVOKABLE QVariantMap setFilter(const QString& ids, const QString& data,
                                      int direction, int flag,
                                      const QString& lengths = QString(),
                                      const QString& times = QString());
    int filterTerms() const { return m_filter.terms(); }
    // The flags a row shows, in words, the ID's width left to its column:
    // "FD BRS", "reassembled", "truncated", "after a loss", "error",
    // "sent", "remote"; "" for none
    static QString flagWords(uint32_t flags);

    // The frames the filter passes, oldest first, as the By ID view and an
    // export read them
    struct Shown {
        uint32_t id;
        uint32_t flags;
        quint64 timestampUs;
        const uint8_t* data;
        int length;
    };
    template <typename F> void forEachShown(F&& f) const
    {
        for (int age = 0; age < m_count; ++age) {
            const Row& r = m_ring[(m_oldest + static_cast<size_t>(age)) % m_ring.size()];
            if (!m_filter.active() || shows(r)) {
                f(Shown{r.arbId, r.flags, r.timestampUs, r.data(), r.length});
            }
        }
    }
    // The newest frame's time, 0 with none held
    quint64 newestTimestampUs() const;

    // The text and hex defs of each session's set, copied and kept, so an
    // export after the session ends still leaves their messages out
    void noteTextDefs(const ecuio_signal_set_t* set);
    // Export CSV (ADR-0054's appendix "frames in memory"): a copy of the
    // frames held through ecuio_scrub_frames, then the kept ones the
    // filter shows, oldest first. One comment line of counts, then
    // time_s,id,ext,dir,dlc,flags,data. A failed check is ok false with
    // core's words, never a value, and no bytes
    struct ScrubbedCsv {
        bool ok = false;
        QString words;
        QByteArray bytes;
        int written = 0;
        quint64 identification = 0;
        quint64 textValues = 0;
        quint64 unattributed = 0;
    };
    ScrubbedCsv scrubbedCsv() const;

    // The selection, by view row. A tap toggles a row and makes it the
    // anchor; a shift-click selects every row from the anchor to its own
    // (a toggle when the anchor has left the ring)
    int selectedCount() const { return static_cast<int>(m_selected.size()); }
    // A view row's arrival number; -1 for a row that isn't there. A table
    // keeps its place by it: rows come and go while it's hidden, and a
    // frame's time can step back (a vendor's stamp that wraps)
    Q_INVOKABLE double seqAt(int row) const;
    // The view row of that arrival, or with it gone or filtered out the
    // next row in the view's order (the first to arrive after it oldest
    // first, before it newest first), the last row past them all; -1 with
    // no rows
    Q_INVOKABLE int rowAtSeq(double seq) const;
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectThrough(int row);
    Q_INVOKABLE void clearSelection();
    // The selected rows in view order, a header line first, one line
    // each, tab-separated: time in seconds to the microsecond, DLC, "E"
    // for an extended ID or "-", the ID in hex and the payload. Empty
    // with nothing selected. The page puts it on the clipboard
    Q_INVOKABLE QString selectedText() const;

    // A payload up to this many bytes lives in its row: every reply the
    // first vehicle gave (27 bytes at most) and every mock frame
    static constexpr int InlinePayload = 64;

    // Test seams: the ring's slots, and the heap bytes the rows hold
    size_t slotsForTesting() const { return m_ring.size(); }
    size_t spilledBytesForTesting() const;

signals:
    void countChanged(int newCount);
    void newestFirstChanged();
    void selectionChanged();
    void filterChanged();

private:
    struct Row {
        quint64 seq = 0;   // arrival number: the ring's rows run consecutively
        quint64 timestampUs = 0;
        quint32 arbId = 0;
        quint32 flags = 0; // ECUIO_FRAME_FLAG_*
        quint16 length = 0;
        bool extended = false;
        uint8_t inlineData[InlinePayload] = {};
        // Only a payload past InlinePayload; a short frame taking the
        // slot frees it, so long replies hold memory only while shown
        std::vector<uint8_t> spill;
        const uint8_t* data() const
        {
            return spill.empty() ? inlineData : spill.data();
        }
    };

    const Row& rowAt(int row) const;
    // The view row of the frame that arrived seq-th; -1 when it's gone
    // or the filter doesn't show it
    int rowOfSeq(quint64 seq) const;
    void notifySelected(int first, int last);
    bool shows(const Row& r) const
    {
        return m_filter.matches(r.arbId, r.flags, r.timestampUs, r.data(),
                                r.length);
    }
    // The ring's row for an arrival number it holds
    const Row& rowOfArrival(quint64 seq) const
    {
        const quint64 oldestSeq = m_nextSeq - static_cast<quint64>(m_count);
        return m_ring[(m_oldest + static_cast<size_t>(seq - oldestSeq)) % m_ring.size()];
    }

    // Grows to MaxFrames and keeps its slots across clear(), so the ring
    // stops allocating once it has gone round
    std::vector<Row> m_ring;
    size_t m_oldest = 0;       // the oldest row's slot
    int m_count = 0;           // rows shown, at most m_ring.size()
    bool m_newestFirst = true;
    quint64 m_nextSeq = 0;     // the next frame's arrival number
    std::set<quint64> m_selected;   // arrival numbers; ordered, so a
                                    // frame leaving the ring is the first
    quint64 m_anchorSeq = 0;
    bool m_hasAnchor = false;
    FrameFilter m_filter;
    // While the filter is active, the arrival numbers of the frames it
    // shows, oldest first
    std::deque<quint64> m_shown;
    // noteTextDefs's copies, each once
    std::vector<ecuio_signal_def_t> m_textDefs;
};

// The Log page's By ID view: one row per ID (and extended or not) over
// the frames FrameListModel shows: how many the table holds, the rate
// over its last 10 s, and the newest payload. refresh() reads the frames
// again, which the page does twice a second while the view shows; rows
// keep their places, an ID that comes or goes inserted or removed, so
// the view doesn't jump.
class FrameIdModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit FrameIdModel(const FrameListModel* frames, QObject* parent = nullptr);

    // The window the rate counts over
    static constexpr quint64 RateWindowUs = 10000000;

    enum IdRoles {
        ArbIdRole = Qt::UserRole + 1,
        ExtendedRole,
        CountRole,
        RateRole,    // frames a second, a double
        PayloadRole, // the newest payload, spaced hex
        ChangedRole  // the newest payload's byte positions that differ from
                     // the frame before it (a list of ints; past the
                     // shorter payload, every byte); none with one frame
    };

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(m_rows.size()); }
    Q_INVOKABLE void refresh();

signals:
    void countChanged();

private:
    struct IdRow {
        quint64 key = 0;    // extended in bit 32, then the ID: the sort order
        int count = 0;
        double rate = 0.0;
        QByteArray payload; // the newest
        QVariantList changed;
    };
    const FrameListModel* m_frames;
    std::vector<IdRow> m_rows;
};
