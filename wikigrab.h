#ifndef WIKI_H
#define WIKI_H

#include <stdint.h>

#define WIKIGRAB_BUILD "0.0.2"

#define clear_struct(s) memset((s), 0, sizeof((*s)))

#define DEFAULT_TMP_BUF_SIZE 16384
#define DEFAULT_MAX_LINE_SIZE 1024

uint32_t runtime_options;

#define option_set(o) ((o) & runtime_options)
#define set_option(o) (runtime_options |= (o))
#define unset_option(o) (runtime_options &= ~(o))

#define OPT_USE_TLS 0x1
#define OPT_OUT_TTY 0x2 /* Print the parsed file to stdout */
#define OPT_REQ_HEADER 0x4 /* Print HTTP request header(s) */
#define OPT_RES_HEADER 0x8 /* Print HTTP response header(s) */

#endif /* !defined WIKI_H */
