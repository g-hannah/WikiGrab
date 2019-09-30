#ifndef WIKI_H
#define WIKI_H

#include <stdint.h>

#define WIKIGRAB_BUILD "0.0.4"
#define WIKIGRAB_DIR "/Wiki_Articles"

#define clear_struct(s) memset((s), 0, sizeof((*s)))

#define __ALIGN(b) (((b) + 0xf) & ~(0xf))

#define DEFAULT_TMP_BUF_SIZE 16384
#define DEFAULT_MAX_LINE_SIZE 1024

int SOCK_SET_FLAG_ONCE;
int SOCK_SSL_SET_FLAG_ONCE;

uint32_t runtime_options;

#define option_set(o) ((o) & runtime_options)
#define set_option(o) (runtime_options |= (o))
#define unset_option(o) (runtime_options &= ~(o))

#define OPT_USE_TLS 0x1
#define OPT_OUT_TTY 0x2 /* Print the parsed file to stdout */
#define OPT_REQ_HEADER 0x4 /* Print HTTP request header(s) */
#define OPT_RES_HEADER 0x8 /* Print HTTP response header(s) */
#define OPT_OPEN_FINISH 0x10 /* Open the article when done */
#define OPT_FORMAT_TXT 0x20 /* format in plain text file */
#define OPT_FORMAT_XML 0x40 /* format in XML file */

#endif /* !defined WIKI_H */
