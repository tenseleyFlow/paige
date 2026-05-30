#ifndef PAIGE_SEARCH_H
#define PAIGE_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

bool paige_search_smart_case(const char *needle, size_t nlen);
bool paige_search_find_forward(const char *hay, size_t hlen, const char *needle,
                               size_t nlen, size_t start, bool case_sensitive,
                               size_t *out_off);
bool paige_search_find_backward(const char *hay, size_t hlen,
                                const char *needle, size_t nlen, size_t before,
                                bool case_sensitive, size_t *out_off);

#endif /* PAIGE_SEARCH_H */
