// File/layer: core/src/uri_query.h | internal, C++17, Qt-free, header-only
// Testing: UNIT — CoreTest's profile-query and mock-query cases;
//          HARNESS — J2534QueryParamTest's malformed-query cases
//
// The one URI query grammar. core.cpp reads ?profile= with it before any
// transport attaches, and each transport reads its own keys with it, so
// every scheme splits and rejects queries the same way.

#ifndef ECUIO_URI_QUERY_H
#define ECUIO_URI_QUERY_H

#include <cstddef>
#include <cstring>

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

} // namespace ecuio_uri

#endif /* ECUIO_URI_QUERY_H */
