#include "search.h"

#include <string.h>

static unsigned char ascii_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return (unsigned char)(c + ('a' - 'A'));
    return c;
}

bool paige_search_smart_case(const char *needle, size_t nlen)
{
    for (size_t i = 0; i < nlen; i++) {
        unsigned char c = (unsigned char)needle[i];
        if (c >= 'A' && c <= 'Z')
            return true;
    }
    return false;
}

static bool match_at(const char *hay, const char *needle, size_t nlen,
                     bool case_sensitive)
{
    for (size_t i = 0; i < nlen; i++) {
        unsigned char a = (unsigned char)hay[i];
        unsigned char b = (unsigned char)needle[i];
        if (!case_sensitive) {
            a = ascii_lower(a);
            b = ascii_lower(b);
        }
        if (a != b)
            return false;
    }
    return true;
}

bool paige_search_find_forward(const char *hay, size_t hlen, const char *needle,
                               size_t nlen, size_t start, bool case_sensitive,
                               size_t *out_off)
{
    if (nlen == 0 || nlen > hlen || start > hlen - nlen)
        return false;
    size_t limit = hlen - nlen; /* last index where a match can begin */

    /* Skip to candidate first bytes with memchr (vectorized in libc) instead of
     * calling match_at on every offset. For a case-insensitive search the first
     * byte can be either case, so we look for the nearer of the two. */
    unsigned char n0 = (unsigned char)needle[0];
    unsigned char n0l = ascii_lower(n0);
    unsigned char n0u =
        (n0l >= 'a' && n0l <= 'z') ? (unsigned char)(n0l - ('a' - 'A')) : n0l;

    for (size_t i = start; i <= limit;) {
        size_t span = limit - i + 1;
        const unsigned char *cand;
        if (case_sensitive) {
            cand = memchr(hay + i, n0, span);
        } else {
            const unsigned char *cl = memchr(hay + i, n0l, span);
            const unsigned char *cu =
                (n0u != n0l) ? memchr(hay + i, n0u, span) : NULL;
            cand = !cl ? cu : (!cu ? cl : (cl < cu ? cl : cu));
        }
        if (!cand)
            return false;
        size_t pos = (size_t)((const char *)cand - hay);
        if (match_at(hay + pos, needle, nlen, case_sensitive)) {
            *out_off = pos;
            return true;
        }
        i = pos + 1;
    }
    return false;
}

bool paige_search_find_backward(const char *hay, size_t hlen,
                                const char *needle, size_t nlen, size_t before,
                                bool case_sensitive, size_t *out_off)
{
    if (nlen == 0 || nlen > hlen || before == 0)
        return false;
    size_t max = hlen - nlen;
    if (before <= max)
        max = before - 1;
    for (size_t i = max + 1; i-- > 0;) {
        if (match_at(hay + i, needle, nlen, case_sensitive)) {
            *out_off = i;
            return true;
        }
        if (i == 0)
            break;
    }
    return false;
}
