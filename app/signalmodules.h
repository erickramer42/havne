#pragma once

#include <QString>

#include <vector>

#include "ecuio/signals.h"

namespace signal_modules {

struct Row {
    int module = -1;
    QString moduleName;
    QString part;
    QString tag;
};

// The file's entry's name, else its id, else the reply ID; "" past the set's
QString nameOf(const ecuio_signal_set_t* set, const ecuio_signal_file_t* file,
               uint32_t module);

// One per def of set, in its order; file may be nullptr (a built-in set)
std::vector<Row> of(const ecuio_signal_set_t* set,
                    const ecuio_signal_file_t* file,
                    const std::vector<ecuio_signal_key_t>& keys);

} // namespace signal_modules
