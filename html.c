#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "cache.h"
#include "html.h"
#include "types.h"
#include "utils.h"
#include "wikigrab.h"

static content_t *content = NULL;

#define mark_start(p) ((*p) = 0x01)
#define mark_end(p) ((*(p-1)) = 0x02)

int
html_get_all(wiki_cache_t *cachep, buf_t *buf, const char *open_pattern, const char *close_pattern)
{
	assert(cachep);
	assert(buf);
	assert(open_pattern);
	assert(close_pattern);

	char *p;
	char *savep;
	char *start;
	char *end;
	char *search_from;
	size_t len;
	int depth;

while (1)
{
	savep = buf->buf_head;
	start = strstr(savep, open_pattern);
	if (!start || start >= buf->buf_tail)
		break;

	search_from = savep = (start + 1);
	end = strstr(search_from, close_pattern);
	if (!end || end > buf->buf_tail)
		break;
	
	while (1)
	{
		depth = 0;
		while (1)
		{
			p = strstr(savep, open_pattern);
			if (!p || p >= end)
				break;

			++depth;
			savep = ++p;
			if (p >= end)
				break;
		}

		if (!depth)
			break;

		search_from = savep = (end + 1);
		while (depth)
		{
			p = strstr(savep, close_pattern);
	
			if (!p || p >= buf->buf_tail)
				break;

			--depth;
			end = p;
			savep = ++p;
			if (p >= buf->buf_tail)
				break;
		}
	}

	if (end)
	{
		p = memchr(end, '>', (buf->buf_tail - end));
		if (p)
		{
			end = ++p;
		}
		else
		{
			end += strlen(close_pattern);
		}

		if (end > buf->buf_tail)
			end = buf->buf_tail;

		fprintf(stderr, "Got content:\n\n%.*s\n\n", (int)(end - start), start);
		fprintf(stderr, "Length: %lu bytes (offset from start of buffer: %lu)\n", (end - start), (start - buf->buf_head));

		content = wiki_cache_alloc(cachep, &content);

		if (!content)
			break;

		len = (end - start);
		if (len >= content->alloc_len)
		{
			content->data = realloc(content->data, __ALIGN(len+1));
			if (!content->data)
				goto fail;
			content->alloc_len = __ALIGN(len+1);
		}

		memcpy((void *)content->data, (void *)start, len);
		content->data[len] = 0;
		content->data_len = len;
		content->off = (off_t)(start - buf->buf_head);

		mark_start(start);
		mark_end(end);
	}
}

	while (1)
	{
		start = memchr(buf->buf_head, 0x01, buf->data_len);

		if (!start)
			break;

		end = memchr(start, 0x02, (buf->buf_tail - start));

		if (!end)
			break;

		++end;
		buf_collapse(buf, (off_t)(start - buf->buf_head), (end - start));
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

	assert((p - savep) < 8192);
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

	assert((p - savep) < 8192);
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
	int depth = 0;

	savep = buf->buf_head;

	while (1)
	{
		savep = buf->buf_head;
		begin = strstr(savep, open_tag);
		if (!begin || begin >= tail)
			break;

		search_from = (begin + 1);

		final = strstr(search_from, close_tag);

		if (!final || final >= tail)
			break;

		while (1)
		{
			savep = search_from;
			depth = 0;

			while (1)
			{
				p = strstr(savep, open_tag);

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
				{
					final = tail;
					break;
				}

				--depth;
				final = p;
				savep = ++p;
			}
		}

		p = memchr(final, '>', (tail - final));

		if (final < tail)
		{
			if (!p)
			{
				final += strlen(close_tag);
			}
			else
			{
				final = ++p;
			}

			if (final > tail)
				final = tail;
		}
		else
		{
			final = tail;
		}

		buf_collapse(buf, (off_t)(begin - buf->buf_head), (final - begin));
		tail = buf->buf_tail;
	}

	return;
}

/**
 * html_remove_elements_class - remove all HTML content with particular class
 * @buf: the buffer holding the HTML data
 * @class: the classname to search for
 */
void
html_remove_elements_class(buf_t *buf, const char *classname)
{
	assert(buf);
	assert(classname);

	char *p;
	char *e;
	char *savep;
	char *tail;
	char *left_angle;
	char *right_angle;
	char *content_end;
	char *search_from;
	int got_tag = 0;
	buf_t open_tag;
	buf_t close_tag;
	int depth = 0;

	savep = buf->buf_head;
	tail = buf->buf_tail;

	buf_init(&open_tag, 64);
	buf_init(&close_tag, 64);

	while (1)
	{
		p = strstr(savep, classname);

		if (!p || p >= tail)
			break;

		if (strncmp("class=", p - strlen("class=\""), 6))
		{
			savep = ++p;
			continue;
		}

		left_angle = p;
		while (*left_angle != '<' && left_angle > (buf->buf_head + 1))
			--left_angle;

		if (!got_tag)
		{
			e = memchr(left_angle, ' ', (p - left_angle));

			if (!e)
				break;

			buf_append_ex(&open_tag, left_angle, (e - left_angle));
			*(open_tag.buf_tail) = 0;
			buf_append(&close_tag, "</");
			buf_append_ex(&close_tag, (left_angle + 1), ((e - left_angle) - 1));
			*(close_tag.buf_tail) = 0;

			got_tag = 1;
		}

		content_end = strstr(p, close_tag.buf_head);
		if (!content_end || content_end >= tail)
			break;

		depth = 0;
		search_from = savep = p;

		while (1)
		{
			while (1)
			{
				p = strstr(savep, open_tag.buf_head);

				if (!p || p >= content_end)
					break;

				++depth;
				savep = ++p;
			}

			if (!depth)
				break;

			search_from = savep = (content_end + 1);

			while (depth)
			{
				p = strstr(savep, close_tag.buf_head);

				if (!p || p >= tail)
					break;

				--depth;
				content_end = p;
				savep = ++p;
			}

			savep = search_from;
		}

		right_angle = memchr(content_end, '>', (tail - content_end));

		if (right_angle)
			content_end = (right_angle + 1);

		buf_collapse(buf, (off_t)(left_angle - buf->buf_head), (content_end - left_angle));
		tail = buf->buf_tail;
		savep = left_angle;
	}

	buf_destroy(&open_tag);
	buf_destroy(&close_tag);

	return;
}

/**
 * html_remove_elements_id - remove HTML content with particular id
 * @buf: the buffer with the HTML data
 * @id: the id to search for
 */
void
html_remove_elements_id(buf_t *buf, const char *id)
{
	assert(buf);
	assert(id);

	char *p;
	char *e;
	char *savep;
	char *tail;
	char *left_angle;
	char *right_angle;
	char *content_end;
	char *search_from;
	int got_tag = 0;
	size_t range;
	buf_t open_tag;
	buf_t close_tag;
	int depth = 0;

	savep = buf->buf_head;
	tail = buf->buf_tail;

	buf_init(&open_tag, 64);
	buf_init(&close_tag, 64);

	while (1)
	{
		p = strstr(savep, id);

		if (!p || p >= tail)
			break;

		if (strncmp("id=", p - strlen("id\""), 3))
		{
			savep = ++p;
			continue;
		}

		left_angle = p;
		while (*left_angle != '<' && left_angle > (buf->buf_head + 1))
			--left_angle;

		if (!got_tag)
		{
			e = memchr(left_angle, ' ', (p - left_angle));

			if (!e)
				break;

			buf_append_ex(&open_tag, left_angle, (e - left_angle));
			*(open_tag.buf_tail) = 0;
			buf_append(&close_tag, "</");
			buf_append_ex(&close_tag, (left_angle + 1), ((e - left_angle) - 1));
			*(close_tag.buf_tail) = 0;

			got_tag = 1;
		}

		content_end = strstr(p, close_tag.buf_head);
		if (!content_end || content_end >= tail)
			break;

		depth = 0;
		search_from = savep = p;

		while (1)
		{
			while (1)
			{
				p = strstr(savep, open_tag.buf_head);

				if (!p || p >= content_end)
					break;

				++depth;
				savep = ++p;
			}

			if (!depth)
				break;

			search_from = savep = (content_end + 1);

			while (depth)
			{
				p = strstr(savep, close_tag.buf_head);

				if (!p || p >= tail)
					break;

				--depth;
				content_end = p;
				savep = ++p;
			}

			savep = search_from;
		}

		right_angle = memchr(content_end, '>', (tail - content_end));
		content_end = (right_angle + 1);
		range = (content_end - left_angle);
		buf_collapse(buf, (off_t)(left_angle - buf->buf_head), range);
		tail = buf->buf_tail;
		savep = left_angle;
	}

	buf_destroy(&open_tag);
	buf_destroy(&close_tag);

	return;
}
