#ifndef PARSE_H
#define PARSE_H

#include "buffer.h"

#define LEFT_ALIGN_WIDTH	28
#define WIKI_ARTICLE_LINE_LENGTH 72
#define MAX_VALUE_LEN 1024

#define XML_START_LINE "<?xml version=\"1.0\" ?>"
#define BEGIN_PARA_MARK "_BEGIN_PARA_"
#define END_PARA_MARK "_END_PARA_"

typedef struct value_t value_t;

struct value_t
{
	char *value;
	size_t vlen;
};

struct article_header
{
	value_t *title;
	value_t *server_name;
	value_t *server_ipv4;
	value_t *server_ipv6;
	value_t *generator;
	value_t *lastmod;
	value_t *downloaded;
	value_t *content_len;
};

int extract_wiki_article(buf_t *);

#endif /* !defined PARSE_H */
