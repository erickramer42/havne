// File/layer: core/src/probes.h | C++17, Qt-free, header-only
// Testing: UNIT — ProbeNotesTest (the probes in the ELF notes, Linux with
//          <sys/sdt.h>); HARNESS — CI's linux-build bpftrace step (each
//          probe fires, on a havne-cli mock:// stream)
//
// USDT probes in core (ADR-0034 §4): named points a tracer (bpftrace,
// perf) can attach to in a running process. On Linux with systemtap's
// header-only <sys/sdt.h> (CMake sets ECUIO_HAVE_SDT), each is one nop
// plus a .note.stapsdt entry naming it and where its arguments live;
// elsewhere they expand to nothing. Provider "ecuio". The names and
// their arguments are what tracing scripts depend on: a change goes
// through ADR-0034.
//
//   session_start  (session, backend)                       the worker runs
//   session_close  (session)                                the worker joined
//   frame          (session, id, flags, length, timestamp_us)  before callbacks

#ifndef ECUIO_PROBES_H
#define ECUIO_PROBES_H

#if defined(ECUIO_HAVE_SDT)
#include <sys/sdt.h>
#define ECUIO_PROBE1(name, a) STAP_PROBE1(ecuio, name, a)
#define ECUIO_PROBE2(name, a, b) STAP_PROBE2(ecuio, name, a, b)
#define ECUIO_PROBE5(name, a, b, c, d, e) STAP_PROBE5(ecuio, name, a, b, c, d, e)
#else
#define ECUIO_PROBE1(name, a) ((void)0)
#define ECUIO_PROBE2(name, a, b) ((void)0)
#define ECUIO_PROBE5(name, a, b, c, d, e) ((void)0)
#endif

#endif
