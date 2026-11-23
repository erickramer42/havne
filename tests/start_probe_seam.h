// File/layer: tests/start_probe_seam.h | test support, header-only
// The start's emissions probe's switch (internal.h's ecuio_set_start_probe,
// request_probe.h): off for a binary whose cases pin a fake's first writes,
// filters or commands (StartProbeOff, a global environment), back on for a
// case of the probe's own (StartProbe, for its scope).
// Testing: support for the J2534 suites

#ifndef ECUIO_TESTS_START_PROBE_SEAM_H
#define ECUIO_TESTS_START_PROBE_SEAM_H

#include "ecuio/internal.h"

#include <gtest/gtest.h>

class StartProbeOff : public ::testing::Environment
{
public:
    void SetUp() override { ecuio_set_start_probe(0); }
    void TearDown() override { ecuio_set_start_probe(1); }
};

struct StartProbe {
    StartProbe() { ecuio_set_start_probe(1); }
    ~StartProbe() { ecuio_set_start_probe(0); }
    StartProbe(const StartProbe&) = delete;
    StartProbe& operator=(const StartProbe&) = delete;
};

#endif
