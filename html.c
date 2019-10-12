#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "cache.h"
#include "html.h"
#include "utils.h"
#include "wikigrab.h"

static content_t *content = NULL;

int
html_get_all(wiki_cache_t *cachep, buf_t *buf, const char *open_pattern, const char *close_pattern)
{
	assert(cachep);
	assert(buf);
	assert(open_pattern);
	assert(close_pattern);

	char *savep = buf->buf_head;
	char *tail = buf->buf_tail;
	char *start;
	char *end;
	size_t len;

	while(1)
	{
		start = strstr(savep, open_pattern);

		if (!start || start >= tail)
			break;

		savep = start;
		end = strstr(savep, close_pattern);

		if (!end || end >= tail)
			break;

		savep = end;
		end = memchr(savep, 0x3e, (tail - savep));

		if (!end)
			break;

		++end;
		content = wiki_cache_alloc(cachep, &content);

		if (!content)
			goto fail;

		len = (end - start);

		if (len >= content->alloc_len)
		{
#ifdef DEBUG
			printf("reallocating memory @ %p\n", content->data);
#endif
			if (!(content->data = realloc(content->data, len+1)))
				goto fail;

			content->alloc_len = len+1;
#ifdef DEBUG
			printf("memory is now at @ %p\n", content->data);
#endif
		}

		assert((end - start) < content->alloc_len);
		strncpy(content->data, start, len);
		content->data_len = len;
		content->off = (off_t)(start - buf->buf_head);
		buf_collapse(buf, (off_t)(start - buf->buf_head), (end - start));
		end = start;
		tail = buf->buf_tail;

		savep = end;

		if (savep >= tail)
			break;

		content = NULL;
	}

	return 0;

	fail:
	return -1;
}

static char tag_content[8192];

char *
html_get_tag_field(buf_t *buf, const char *tag, const char *field)
{
	assert(buf);
	assert(tag);
	assert(field);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;

	p = strstr(buf->buf_head, tag);

	if (!p)
		return NULL;

	savep = p;

	p = strstr(savep, field);

	if (!p)
		return NULL;

	savep = p;

	/* e.g. "content=\"..."
	 *       s         p
	 */
	p += (strlen(field) + 2);

	savep = p;

	p = memchr(savep, 0x22, (tail - savep));

	if (!p)
		return NULL;

	strncpy(tag_content, savep, (p - savep));
	tag_content[p - savep] = 0;

	return tag_content;
}

char *
html_get_tag_content(buf_t *buf, const char *tag)
{
	assert(buf);
	assert(tag);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;

	p = strstr(buf->buf_head, tag);

	if (!p)
		return NULL;

	savep = p;

	p = memchr(savep, 0x3e, (tail - savep));

	if (!p)
		return NULL;

	savep = ++p;

	p = memchr(savep, 0x3c, (tail - savep));

	if (!p)
		return NULL;

	strncpy(tag_content, savep, (p - savep));
	tag_content[p - savep] = 0;

	return tag_content;
}

void
html_remove_content(buf_t *buf, char *open_tag, char *close_tag)
{
	assert(buf);
	assert(open_tag);
	assert(close_tag);

	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	char *search_from;
	char *begin;
	char *final;
	buf_t otag_part;
	int depth = 0;

	savep = buf->buf_head;
	buf_init(&otag_part, 64);

	p = open_tag;
	savep = memchr(p, ' ', strlen(open_tag));
	buf_append_ex(&otag_part, p, (savep - p));
	*(otag_part.buf_tail) = 0;

while (1)
{
	savep = buf->buf_head;
	begin = strstr(savep, open_tag);
	if (!begin || begin >= tail)
		break;
		//goto out_destroy_buf;

	search_from = (begin + 1);

	final = strstr(search_from, close_tag);

	if (!final || final >= tail)
		break;
		//goto out_destroy_buf;

	while (1)
	{
		savep = search_from;
		depth = 0;

		while (1)
		{
			p = strstr(savep, otag_part.buf_head);

			if (!p || p >= final)
				break;

			++depth;
			savep = ++p;
		}

		if (!depth)
			break;

		savep = search_from = (final + 1);
		while (depth)
		{
			p = strstr(savep, close_tag);

			if (!p || p >= tail)
				break;

			--depth;
			final = p;
			savep = ++p;
		}
	}

	p = memchr(final, '>', (tail - final));
	if (!p)
		final += strlen(close_tag);
	else
		final = ++p;

	buf_collapse(buf, (off_t)(begin - buf->buf_head), (final - begin));
}

	buf_destroy(&otag_part);
	return;
}
