#ifndef UTILS_H
#define UTILS_H 1

#include <sys/types.h>
#include "buffer.h"

char *nested_closing_char(char *, char *, char, char) __nonnull((1,2)) __wur;
void remove_excess_sp(buf_t *) __nonnull((1));

#endif /* !defined UTILS_H */
