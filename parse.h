#ifndef PARSE_H
#define PARSE_H

#include "buffer.h"

#define LEFT_ALIGN_WIDTH	20
#define WIKI_ARTICLE_LINE_LENGTH 72
#define MAX_VALUE_LEN 1024

#define XML_START_LINE		"<?xml version=\"1.0\" ?>"
#define BEGIN_PARA_MARK		"_BEGIN_PARA_"
#define END_PARA_MARK			"_END_PARA_"
#define BEGIN_ULIST_MARK	"_BEGIN_ULIST_"
#define END_ULIST_MARK		"_END_ULIST_"
#define BEGIN_LIST_MARK		"_BEGIN_LIST_"
#define END_LIST_MARK			"_END_LIST_"

int extract_wiki_article(buf_t *);

#endif /* !defined PARSE_H */
