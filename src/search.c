#include "search.h"

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

bool paige_search_find_forward(const char *hay, size_t hlen,
                               const char *needle, size_t nlen, size_t start,
                               bool case_sensitive, size_t *out_off)
{
    if (nlen == 0 || nlen > hlen || start > hlen - nlen)
        return false;
    for (size_t i = start; i <= hlen - nlen; i++) {
        if (match_at(hay + i, needle, nlen, case_sensitive)) {
            *out_off = i;
            return true;
        }
    }
    return false;
}

bool paige_search_find_backward(const char *hay, size_t hlen,
                                const char *needle, size_t nlen,
                                size_t before, bool case_sensitive,
                                size_t *out_off)
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
