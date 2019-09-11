#ifndef PARSE_H
#define PARSE_H

#include "buffer.h"

#define LEFT_ALIGN_WIDTH	28
#define WIKI_ARTICLE_LINE_LENGTH 72
#define MAX_VALUE_LEN 1024

#define XML_START_LINE		"<?xml version=\"1.0\" ?>"
#define BEGIN_PARA_MARK		"_BEGIN_PARA_"
#define END_PARA_MARK			"_END_PARA_"
#define BEGIN_ULIST_MARK	"_BEGIN_ULIST_"
#define END_ULIST_MARK		"_END_ULIST_"
#define BEGIN_LIST_MARK		"_BEGIN_LIST_"
#define END_LIST_MARK			"_END_LIST_"

typedef struct value_t value_t;
typedef struct content_t content_t;

struct value_t
{
	char *value;
	size_t vlen;
};

struct content_t
{
	char *data;
	size_t data_len;
	size_t alloc_len;
	off_t off;
};

struct offset_idx
{
	off_t off;
	int idx;
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
