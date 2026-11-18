// File/layer: app/variantshape.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.VariantShape* (test_bridge, tests/test_bridge_variantshape.cpp); QML: QmlTest.settings_rows

#pragma once

#include <QString>
#include <QStringList>

#include "ecuio/signals.h"

// What a signal file adds to the built-in set it extends, for the stores'
// rows (the Signals list's "obd2, Boost replaced, 2 more"). Core puts a file's
// signal named as a base signal in the base's place (the base's is left
// out of the set) and the file's own after the set's rest, so the set's
// count less the base's counts only what the file adds beyond what it
// replaces: the shape is judged by name against the base here, one place
// for both stores.
namespace variant_shape {

struct Shape
{
    int count = 0;         // the set's signals, the base's included
    int added = 0;         // the file's own signals the base has no name for
    QStringList replaced;  // the base signals the file's own replaces, by
                           // name, in the file's order
};

// A file's shape against base (null: it extends nothing, every signal of
// the file is its own). A replacement that is the base's signal over
// again, in everything sameDef reads, is neither added nor replaced; one
// that gives its def a gauge is replaced, the base's defs having none.
Shape of(const ecuio_signal_file_t* file, const ecuio_signal_set_t* base);

// Whether two defs decode and show the same: what the decoder and a
// display read (match, mux, kind, byte order, field, scale, offset, name,
// unit, decimals, plot rank, group, flags, an enum's entries, a computed
// def's program). The id is the same by core's rule and the reserved words
// are zero.
bool sameDef(const ecuio_signal_set_t& aSet, const ecuio_signal_def_t& a,
             const ecuio_signal_set_t& bSet, const ecuio_signal_def_t& b);

} // namespace variant_shape
