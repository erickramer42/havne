// File/layer: core/src/uri_query.h | internal, C++17, Qt-free, header-only
// Testing: UNIT — CoreTest's profile-query and mock-query cases;
//          HARNESS — J2534QueryParamTest's malformed-query cases
//
// The one URI query grammar. ecuio_session_create splits the query once,
// refuses any key its scheme's transport doesn't read and reads ?profile=,
// all before any transport attaches; the transport then reads its values
// from the split query. So every scheme splits and rejects queries the
// same way, in one place.

#ifndef ECUIO_URI_QUERY_H
#define ECUIO_URI_QUERY_H

#include <cstddef>
#include <cstring>

/* The most segments a query may have: every key of the busiest scheme,
 * and a repeat of most of them. */
constexpr size_t ECUIO_URI_MAX_PARAMS = 16;

namespace ecuio_uri {

/* One key=value segment, as views into the URI. */
struct param {
    const char* key;
    size_t key_len;
    const char* val;
    size_t val_len;

    /* Keys match exactly, so they're case-sensitive (lowercase by
     * convention, ADR-0013 §2). */
    bool is(const char* name) const
    {
        return key_len == strlen(name) && strncmp(key, name, key_len) == 0;
    }
};

/* The query of a URI body (the text after the scheme): what follows the
 * first '?', or nullptr when there's no '?'. */
inline const char* query_of(const char* body)
{
    const char* q = strchr(body, '?');
    return (q != nullptr) ? q + 1 : nullptr;
}

/* Split a query into key=value segments joined by '&' and call
 * visit(const param&) for each, in order, until one returns false.
 * Malformed: an empty query (a bare trailing '?'), an empty segment
 * ("&&", a leading or trailing '&') and a segment without '='. The
 * value is everything after the first '='; duplicate keys reach visit
 * in order, so last-wins is the visitor's. A null query (no '?') has
 * no segments. Returns true when every segment parsed and visit
 * accepted it. */
template <typename Visit>
bool for_each_param(const char* query, Visit&& visit)
{
    if (query == nullptr) {
        return true;
    }
    const char* cur = query;
    for (;;) {
        const char* amp = strchr(cur, '&');
        const char* end = (amp != nullptr) ? amp : cur + strlen(cur);
        if (end == cur) {
            return false;   /* empty query or empty segment */
        }
        const char* eq = static_cast<const char*>(
            memchr(cur, '=', static_cast<size_t>(end - cur)));
        if (eq == nullptr) {
            return false;   /* no '=' in the segment */
        }
        const param p = {cur, static_cast<size_t>(eq - cur),
                         eq + 1, static_cast<size_t>(end - (eq + 1))};
        if (!visit(p)) {
            return false;
        }
        if (amp == nullptr) {
            return true;
        }
        cur = amp + 1;
    }
}

enum class split_result { ok, malformed, too_long };

} // namespace ecuio_uri

/* A query split once, its segments in order: views into the URI, valid
 * while the URI is. Declared in internal.h, where the transports' attach
 * functions take it. */
struct ecuio_uri_query {
    ecuio_uri::param params[ECUIO_URI_MAX_PARAMS];
    size_t count;
};

namespace ecuio_uri {

/* Splits query (nullptr: no '?', no segments) into *out, by
 * for_each_param's grammar; too_long past ECUIO_URI_MAX_PARAMS. */
inline split_result split(const char* query, ecuio_uri_query* out)
{
    out->count = 0;
    bool too_long = false;
    const bool ok = for_each_param(query, [&](const param& p) {
        if (out->count == ECUIO_URI_MAX_PARAMS) {
            too_long = true;
            return false;
        }
        out->params[out->count++] = p;
        return true;
    });
    return too_long ? split_result::too_long
         : ok       ? split_result::ok
                    : split_result::malformed;
}

/* The first segment whose key isn't in keys (a nullptr-terminated
 * list), or nullptr when every key is. */
inline const param* first_unknown(const ecuio_uri_query& q,
                                  const char* const* keys)
{
    for (size_t i = 0; i < q.count; ++i) {
        bool known = false;
        for (const char* const* k = keys; *k != nullptr && !known; ++k) {
            known = q.params[i].is(*k);
        }
        if (!known) {
            return &q.params[i];
        }
    }
    return nullptr;
}

} // namespace ecuio_uri

#endif /* ECUIO_URI_QUERY_H */
