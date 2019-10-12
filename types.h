#ifndef TYPES_H
#define TYPES_H 1

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

#endif /* !defined TYPES_H */
