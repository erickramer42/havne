// File/layer: app/readonce.h | Qt6 Core, app layer, C++17
// Testing: UNIT: ReadOnceTest.* (test_bridge): the request URI from an
//          adapter's, the draft's refusals, a reply's bytes, value and
//          NRC words; HARNESS: BridgeTest.ReadOnce* against the fake
//          J2534 DLL's ECU (a DID, a PID, a refusal, the idle rule)
//
// Read once (ADR-0051's form): one request for a signal still being
// written, its reply's bytes shown beside the value its draft decodes
// to, so the user sees whether the field and the scaling are right
// before saving. The session only reads while no other runs: a short
// session of its own on the vehicle's adapter, Service 22 through
// uds-generic's ?did= and Service 01 through the raw profile's ?poll=,
// closed after the first reply or the wait. Core decodes the reply with
// a def built from the draft (ecuio_decode_signal, ecuio_decode_text);
// the app only builds the def and words what came back.

#pragma once

#include <QString>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "ecuio/signals.h"

class ReadOnce
{
public:
    // A draft read: the request, the module and the def its reply
    // decodes with
    struct Request {
        uint8_t service = 0; // 0x01 or 0x22
        uint16_t number = 0; // the PID or the DID
        uint32_t rx = 0;     // the module's reply ID
        uint32_t tx = 0;     // its request ID
        ecuio_signal_def_t def{};
        QVariantMap labels; // an enum's labels by decimal raw value
    };

    // The draft as a signal file holds a signal (docs/signal-files.md):
    // { name, kind, read: { service, number }, field: { byte, bits, bit,
    // order, signed, bytes }, scale, offset, unit, decimals, values },
    // and the module as its ecus entry, { rx, tx }. False with *why
    // naming the first key that's wrong.
    static bool requestFrom(const QVariantMap& signal,
                            const QVariantMap& module, Request* out,
                            QString* why);

    // The URI that asks for it on the adapter of adapterUri: its scheme,
    // path and adapter keys, without the keys that say what a session
    // asks for. "" with *why for a URI that names no adapter.
    static QString uriFor(const QString& adapterUri, const Request& r,
                          QString* why);

    // Whether a reply payload from rx answers r: its positive reply, or a
    // negative one other than 78 (response pending)
    static bool answers(const Request& r, uint32_t rx, const uint8_t* data,
                        uint16_t length);

    // What a reply says: { ok, request, reply, value, raw, nrc, words }.
    // request and reply are hex pairs; value the draft's decode, with
    // its unit or label; nrc and words a refusal's code and its ISO
    // 14229 name. ok false and words alone when the decode fails.
    static QVariantMap evaluate(const Request& r, const uint8_t* data,
                                uint16_t length);

    // { ok: false, request, words } for no reply
    static QVariantMap noReply(const Request& r, const QString& words);

    // ISO 14229-1's name for a negative response code, "" for one it
    // doesn't name
    static QString nrcName(uint8_t nrc);

    // The request ID the standard gives a reply ID (rx - 8 for 11-bit
    // 7E8 to 7EF, 18DAxxF1 for 18DAF1xx), 0 where it gives none
    static uint32_t standardTx(uint32_t rx);

    ReadOnce() = default;
    ~ReadOnce();
    ReadOnce(const ReadOnce&) = delete;
    ReadOnce& operator=(const ReadOnce&) = delete;

    bool busy() const { return m_busy.load(std::memory_order_acquire); }

    // Runs uri's session on a thread of its own: the result (evaluate's
    // or noReply's) goes to done, on that thread, after the session has
    // closed. waitMs bounds the wait for a reply once the session
    // starts. False when one is running already.
    bool start(const QString& uri, const Request& r, int waitMs,
               std::function<void(const QVariantMap&)> done);

    // Joins the finished thread; call from done's continuation
    void join();

private:
    std::thread m_thread;
    std::atomic<bool> m_busy{false};
};
