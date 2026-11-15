// File/layer: core/src/condition_file.h | C++17, Qt-free, every platform
// Testing: UNIT: ConditionFileTest (test_signals)
//
// The condition loader's inside (ADR-0063): the set a condition file
// loads into, as the session's evaluator and the recorder read it.

#ifndef ECUIO_CONDITION_FILE_H
#define ECUIO_CONDITION_FILE_H

#include "ecuio/conditions.h"

#include <string>
#include <vector>

struct ecuio_condition_set {
    struct condition {
        std::string key;
        std::string name;
        /* key and name stay NULL here: ecuio_condition_set_get points
         * them at the strings above */
        ecuio_condition_info_t info{};
        ecuio_condition_term_t terms[ECUIO_CONDITION_MAX_TERMS]{};
    };

    std::string name; /* the file's, "" with none */
    ecuio_check_options_t checks{};
    std::vector<condition> conditions;
    /* The bytes ecuio_condition_set_load read: a recording copies them
     * as conditions.json */
    std::string bytes;
};

#endif /* ECUIO_CONDITION_FILE_H */
