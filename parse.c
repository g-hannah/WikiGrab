#include <assert.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "http.h"
#include "parse.h"
#include "wikigrab.h"

#define CONTENT_DATA_SIZE		16384UL

int
sort_content_cache(const void *obj1, const void *obj2)
{
	content_t *c1 = (content_t *)obj1;
	content_t *c2 = (content_t *)obj2;

	return (c1->off - c2->off);
}

int
content_cache_ctor(void *obj)
{
	assert(obj);

	content_t *content = (content_t *)obj;

	if (!(content->data = calloc(CONTENT_DATA_SIZE, 1)))
		return -1;

#ifdef DEBUG
	printf(
		"allocated %lu bytes of memory @ %p (content->data)\n",
		CONTENT_DATA_SIZE, content->data);
#endif

	content->data_len = 0;
	content->alloc_len = CONTENT_DATA_SIZE;
	content->off = 0;

	return 0;
}

void
content_cache_dtor(void *obj)
{
	assert(obj);

	content_t *content = (content_t *)obj;

#ifdef DEBUG
	printf(
		"freeing content_t object:\n"
		"ptr: %p\n"
		"data: %*.*s...\n"
		"len: %lu bytes\n"
		"off: %lu bytes\n",
		content->data,
		(int)20, (int)20, content->data,
		content->data_len,
		content->off);
#endif

	if (content->data)
		free(content->data);

	memset(content, 0, sizeof(*content));

	return;
}

int
value_cache_ctor(void *obj)
{
	assert(obj);

	value_t *val = (value_t *)obj;
	if (!(val->value = calloc(MAX_VALUE_LEN+1, 1)))
		return -1;

	val->vlen = 0;

	return 0;
}

void
value_cache_dtor(void *obj)
{
	assert(obj);

	value_t *val = (value_t *)obj;

	if (val->value)
		free(val->value);

	val->value = NULL;

	return;
}

#define RESET() (p = savep = buf->buf_head)

static void
__remove_html_tags(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;
	off_t poff;

	savep = buf->buf_head;

	while(1)
	{
		p = memchr(savep, 0x3c, (tail - savep));

		if (!p || p >= tail)
			break;

		if (option_set(OPT_FORMAT_XML))
		{
			if (!strncmp(p, "<p", 2)
			|| !strncmp(p, "<ul", 3)
			|| !strncmp(p, "<li", 3)
			|| !strncmp(p, "<table", 6)
			|| !strncmp(p, "<tbody", 6)
			|| !strncmp(p, "<tr", 3)
			|| !strncmp(p, "<td", 3)
			|| !strncmp(p, "<pre", 4)
			|| !strncmp(p, "<math", 5))
			{
				/*
				 * We want to keep the tags, but without the classes, styles, etc.
				 */
				savep = p;

				while (!isspace(*savep) && *savep != 0x3e)
					++savep;

				p = savep;

				if (*p != 0x3e)
				{
					savep = p;

					p = memchr(savep, 0x3e, (tail - savep));

					if (!p)
						break;

					range = (p - savep);

					buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
					tail = buf->buf_tail;
					p = savep;
				}

				continue;
			}
			else
			if (!strncmp("</p", p, 3)
			|| !strncmp("</ul", p, 4)
			|| !strncmp("</li", p, 4)
			|| !strncmp("</table", p, 7)
			|| !strncmp("</tbody", p, 7)
			|| !strncmp("</tr", p, 4)
			|| !strncmp("</td", p, 4)
			|| !strncmp("</pre", p, 5)
			|| !strncmp("</math", p, 6)) 
			{
				savep = ++p;
				continue;
			}
		}
		else
		{
			if (!strncmp("</li", p, 4)
			|| !strncmp("</tr", p, 4)
			|| !strncmp("</td", p, 4)
			|| !strncmp("</ul", p, 4)
			|| !strncmp("</pre", p, 5)
			|| !strncmp("</math", p, 6))
			{
				poff = (off_t)(p - buf->buf_head);
				buf_shift(buf, (off_t)(p - buf->buf_head), (size_t)1);
				tail = buf->buf_tail;
				p = (buf->buf_head + poff);
				strncpy(p, "\n", 1);
				++p;
			}
		}

		savep = p;

		p = memchr(savep, 0x3e, (tail - savep));

		if (!p)
			break;

		++p;

		range = (p - savep);

		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		p = savep;
		tail = buf->buf_tail;
	}

	return;
}

static void
__remove_inline_refs(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	p = savep = buf->buf_head;

	while(1)
	{
		p = strstr(savep, "&#91;");

		if (!p || p >= tail)
			break;

		savep = p;

		p = strstr(savep, "&#93;");

		if (!p)
			p = (savep + strlen("&#91;"));
		else
			p += strlen("&#93;");

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		tail = buf->buf_tail;
		p = savep;

		if (p >= tail)
			break;
	}
}

static void
__remove_html_encodings(buf_t *buf)
{
	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	p = savep = buf->buf_head;

	while(1)
	{
		p = strstr(savep, "&#");

		if (!p || p >= tail)
			break;

		*p++ = 0x20;

		savep = p;

		p = memchr(savep, ';', (tail - savep));

		if (!p)
			p = (savep + 1);
		else
			++p;

		range = (p - savep);
#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
		tail = buf->buf_tail;
		p = savep;
	}
}

struct html_entity_t
{
	char *entity;
	char _char;
};

struct html_entity_t HTML_ENTS[6] =
{
	{ "&quot;", 0x22 },
	{ "&amp;", 0x26 },
	{ "&lt;", 0x3c },
	{ "&gt;", 0x3e },
	{ "&nbsp;", 0x20 },
	{ (char *)NULL, 0 }
};

static void
__replace_html_entities(buf_t *buf)
{
	char *tail = buf->buf_tail;
	char *p;
	char *savep;
	int i;

	if (option_set(OPT_FORMAT_XML))
		return;

	for (i = 0; HTML_ENTS[i].entity != NULL; ++i)
	{
		p = savep = buf->buf_head;

		while (1)
		{
			p = strstr(savep, HTML_ENTS[i].entity);

			if (!p || p >= tail)
				break;

			*p++ = HTML_ENTS[i]._char;
			savep = p;
			p = memchr(savep, ';', (tail - savep));

			if (!p)
				break;

			++p;

			buf_collapse(buf, (off_t)(savep - buf->buf_head), (p - savep));
			tail = buf->buf_tail;
			savep = p;
		}
	}
	return;
}

#if 0
static void
__remove_garbage(buf_t *buf)
{
	buf_t copy_buf;
	char *p;
	char *q;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	buf_init(&copy_buf, DEFAULT_MAX_LINE_SIZE * 2);
	p = savep = buf->buf_head;

	while(1)
	{
		p = memchr(savep, 0x2e, (tail - savep));

		if (!p)
			break;

		savep = p;
		while (!isspace(*p))
			--p;

		q = savep;
		while (!isspace(*q))
			++q;

		range = (q - p);

		buf_append_ex(&copy_buf, p, range);

		if ((*(q-1) == 0x2e) /* End of a line */
		|| (*(q-1) == ')' && *(q-2) == 0x2e) /* End of line in parenthesis */
		|| strstr(copy_buf.buf_head, "&lt;")
		|| strstr(copy_buf.buf_head, "&gt;")
		|| strstr(copy_buf.buf_head, "&quot;")
		|| strstr(copy_buf.buf_head, "&amp;")
		|| strstr(copy_buf.buf_head, "&nbsp;")
		|| (isdigit(*(savep-1)) && isdigit(*(savep+1))) /* e.g., "Version 2.0" */
		|| (!memchr(p, '-', range)
		&& !memchr(p, '{', range)
		&& !memchr(p, '}', range)
		&& !memchr(p, '#', range)
		&& !memchr(p, ';', range)
		&& !memchr(p, '(', range)
		&& !memchr(p, ')', range)
		&& !memchr(p, '-', range)))
		{
			p = savep = q;
			continue;
		}

#ifdef DEBUG
		printf("removing LINE_BEGIN|%.*s|LINE_END\n", (int)range, savep);
#endif
		buf_collapse(buf, (off_t)(p - buf->buf_head), range);
		tail = buf->buf_tail;
		savep = p;
	}

	buf_destroy(&copy_buf);

	return;
}
#endif

/*
 * buf_shift() could require extending the buffer,
 * and this in turn could result in our data
 * being copied to somewhere else in the heap.
 * So save all the offsets and restore them
 * after the shift.
 */
#define BUF_SHIFT_SAFE(by, optr)\
do {\
	size_t __lstart_off;\
	size_t __lend_off;\
	size_t __nloff;\
	size_t __rightoff;\
	size_t __poff;\
	size_t __spoff;\
	if (line_start)\
		__lstart_off = (line_start - buf->buf_head);\
	if (line_end)\
		__lend_off = (line_end - buf->buf_head);\
	if (new_line)\
		__nloff = (new_line - buf->buf_head);\
	if (right)\
		__rightoff = (right - buf->buf_head);\
	if (p)\
		__poff = (p - buf->buf_head);\
	if (savep)\
		__spoff = (savep - buf->buf_head);\
	buf_shift(buf, (off_t)((optr) - buf->buf_head), (by));\
	if (line_start)\
		line_start = (buf->buf_head + __lstart_off);\
	if (line_end)\
		line_end = (buf->buf_head + __lend_off);\
	if (new_line)\
		new_line = (buf->buf_head + __nloff);\
	if (right)\
		right = (buf->buf_head + __rightoff);\
	if (p)\
		p = (buf->buf_head + __poff);\
	if (savep)\
		savep = (buf->buf_head + __spoff);\
} while(0)

static int
__do_format_txt(buf_t *buf)
{
	assert(buf);

	char *tail = NULL;
	char *line_start = NULL;
	char *line_end = NULL;
	char *p = NULL;
	char *savep = NULL;
	char *left = NULL;
	char *right = NULL;
	char *new_line = NULL;
	size_t line_len;
	size_t delta;
	int	gaps = 0;
	int passes;
	int remainder;
	int volte_face = 0;
	size_t ulist_start_len = strlen(BEGIN_ULIST_MARK);
	size_t ulist_end_len = strlen(END_ULIST_MARK);
	size_t list_start_len = strlen(BEGIN_LIST_MARK);
	size_t list_end_len = strlen(END_LIST_MARK);
	size_t range = 0;

	p = buf->buf_head;
	if (*p == 0x0a)
	{
		while (*p == 0x0a && p < tail)
			++p;

		buf_collapse(buf, (off_t)0, (p - buf->buf_head));
		tail = buf->buf_tail;
	}

	tail = buf->buf_tail;
	savep = buf->buf_head;

	while(1)
	{
		p = strstr(savep, BEGIN_ULIST_MARK);
	
		if (!p || p >= tail)
			break;

		buf_collapse(buf, (off_t)(p - buf->buf_head), ulist_start_len);
		tail = buf->buf_tail;

		savep = p;

		p = strstr(savep, END_ULIST_MARK);

		if (!p || p >= tail)
			break;

		buf_collapse(buf, (off_t)(p - buf->buf_head), ulist_end_len);
		tail = buf->buf_tail;
		savep = p;
	}

	savep = buf->buf_head;

	while(1)
	{
		p = strstr(savep, BEGIN_LIST_MARK);

		if (!p || p >= tail)
			break;

		buf_collapse(buf, (off_t)(p - buf->buf_head), list_start_len);
		tail = buf->buf_tail;

		savep = p;

		p = strstr(savep, END_LIST_MARK);

		if (!p || p >= tail)
			break;

		strncpy(p, "\n\n", 2);

		buf_collapse(buf, (off_t)((p - buf->buf_head) + 2), (list_end_len - 2));
		tail = buf->buf_tail;

		savep = p;

		while (*p == 0x0a)
			++p;

		range = (p - savep);

		if (range > 2)
		{
			savep += 2;
			range = (p - savep);
			buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
			tail = buf->buf_tail;
			p = savep;
		}

		savep = p;
	}

	tail = buf->buf_tail;
	line_start = buf->buf_head;

	while (1)
	{
		outer_loop_begin:
		line_end = (line_start + WIKI_ARTICLE_LINE_LENGTH);

		if (line_end > tail)
			line_end = tail;

		if (line_start >= line_end)
			break;

		savep = line_start;

		/*
		 * Remove new lines occuring within our
		 * new line length.
		 */
		while (1)
		{
			if (savep >= line_end)
				break;

			p = memchr(savep, 0x0a, (line_end - savep));

			if (!p)
				break;

		/*
		 * Then it's the end of a paragraph.
		 * Leave it alone and go to 
		 */
			if (*(p+1) == 0x0a)
			{
				while (*p == 0x0a)
					++p;

				line_start = savep = p;
				goto outer_loop_begin;
			}

			*p++ = 0x20;
			savep = p;
		}

		/*
		 * LINE_START + WIKI_ARTICLE_LINE_LENGTH may
		 * happen to already be on a new line.
		 */
		if (*line_end == 0x0a)
		{
			new_line = line_end;

			while (*line_end == 0x0a)
				++line_end;
		}
		else
		if (*line_end == 0x20)
		{
			new_line = line_end;

			*line_end++ = 0x0a;
		}
		else
		if (line_end < tail)
		{
			/*
			 * Find the nearest space backwards.
			 */
			while (*line_end != 0x20 && line_end > (line_start + 1))
				--line_end;

			if (line_end == line_start)
			{
				line_end += WIKI_ARTICLE_LINE_LENGTH;

				BUF_SHIFT_SAFE(1, line_end);
				tail = buf->buf_tail;

				*line_end++ = 0x0a;

				while (*line_end == 0x0a)
					++line_end;

				line_start = line_end;

				continue;
			}

			new_line = line_end;

			*line_end++ = 0x0a;
		}

		if (line_end >= tail)
			line_end = new_line = tail;

		line_len = (new_line - line_start);
		delta = (WIKI_ARTICLE_LINE_LENGTH - line_len);

		/*
		 * Needs justified.
		 */
		if (delta > 0)
		{
			p = savep = line_start;
			gaps = 0;

			while (1)
			{
				p = memchr(savep, 0x20, (new_line - savep));

				if (!p)
					break;

				++gaps;

				while (*p == 0x20)
					++p;

				if (p >= new_line)
					break;

				savep = p;
			}

			if (line_len < (WIKI_ARTICLE_LINE_LENGTH / 3))
			{
				line_start = line_end;
				continue;
			}

			if (!gaps)
			{
				line_start = line_end;
				continue;
			}

			passes = (delta / gaps);
			remainder = (delta % gaps);

			p = savep = line_start;
			while (passes > 0)
			{
				p = memchr(savep, 0x20, (new_line - savep));

				if (!p)
				{
					--passes;
					p = savep = line_start;
					continue;
				}

				BUF_SHIFT_SAFE(1, p);
				tail = buf->buf_tail;
				++line_end;
				++new_line;

				*p++ = 0x20;

				while (*p == 0x20)
					++p;

				if (p >= new_line)
					break;

				savep = p;
			}

			if (remainder)
			{
				left = line_start;
				right = new_line;
				volte_face = 0;

				while (remainder)
				{
					if (!volte_face)
					{
						p = memchr(left, 0x20, (right - left));

						if (!p)
						{
							left = line_start;
							right = (new_line - 1);
							volte_face = 0;
							continue;
						}
					}
					else
					{
						p = right;
						while (*p != 0x20 && p > left)
							--p;

						if (p == left)
						{
							left = line_start;
							right = (new_line - 1);
							volte_face = 0;
							continue;
						}
					}

					BUF_SHIFT_SAFE(1, p);
					tail = buf->buf_tail;

					++line_end;
					++new_line;
					++right;

					*p++ = 0x20;
					--remainder;

					if (!volte_face)
					{
						while (*p == 0x20)
							++p;

						left = p;

						volte_face = 1;
					}
					else
					{
						while (*p == 0x20)
							--p;

						right = p;

						volte_face = 0;
					}
				} /* while(remainder) */
			} /* if (remainder) */
		} /* if (delta > 0) */

		line_start = line_end;

		if (line_end == tail)
			break;
	} /* while(1) */

	__replace_html_entities(buf);

	return 0;
}

static char tag_content[8192];

static char *
__get_tag_content(buf_t *buf, const char *tag)
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

static char *
__get_tag_field(buf_t *buf, const char *tag, const char *field)
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

static content_t *content = NULL;

static int
__get_all(wiki_cache_t *cachep, buf_t *buf, const char *open_pattern, const char *close_pattern)
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

static int
__extract_area(buf_t *sbuf, buf_t *dbuf, char *const open_pattern, char *const close_pattern)
{
	assert(sbuf);
	assert(dbuf);
	assert(open_pattern);
	assert(close_pattern);

	char *p;
	char *savep;
	char *tail = sbuf->buf_tail;
	char *search_from;
	int depth = 0;
	buf_t tmp_buf;
	char *tmp_tail;
	buf_t first_part;
	size_t op_len;
	char *save_start;
	char *save_close;

/*
 * 1. SEARCH FOR OPEN PATTERN.
 * 2. IF NONE FOUND, GOTO END.
 * 3. SEARCH RANGE [OPENPATTERN+1,TAIL) FOR CLOSE PATTERN.
 * 4. IF NONE FOUND, GOTO FAIL
 * 5. SEARCH RANGE [OPENPATTERN+1, CLOSINGPATTERN) FOR MORE OPEN PATTERNS.
 * 6. IF MORE, SEARCH RANGE [CLOSINGPATTERN+1,TAIL) FOR EQUAL # OF CLOSING PATTERNS, ELSE GOTO END.
 * 7. SEARCH RANGE [CLOSINGPATTERN+1,CLOSINGPATTERN') FOR MORE OPEN PATTERNS.
 * 8. GOTO 6.
 * 9. END.
 * 10. FAIL
 *
 */

	buf_init(&tmp_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&first_part, 32);

	p = open_pattern;
	op_len = strlen(open_pattern);
	while (!isspace(*p) && p < (open_pattern + op_len))
		++p;

/*
 * Open_pattern may be something like "<div id=\"mw-content-text\""
 * So extract the "<div" part so we can search ranges for more div tags.
 */
	buf_append_ex(&first_part, open_pattern, (p - open_pattern));

	search_from = sbuf->buf_head;

	p = strstr(search_from, open_pattern);

	if (!p)
		goto out;

	save_start = p;
	search_from = ++p;
	p = strstr(search_from, close_pattern);

	if (!p || p >= tail)
	{
		fprintf(stderr, "__extract_area: failed to find closing tag for \"%s\"\n", open_pattern);
		goto fail;
	}

	save_close = p;

	/*
	 * Copy range [SEARCH_FROM,CLOSINGTAG) into temp buffer
	 * so we can search for more open patterns.
	 */
	buf_append_ex(&tmp_buf, search_from, (save_close - search_from));
	p = savep = tmp_buf.buf_head;
	tmp_tail = tmp_buf.buf_tail;

	while(1)
	{
		/*
		 * Search range in the temp buffer for more open patterns.
		 */
		while(1)
		{
			p = strstr(savep, first_part.buf_head);

			if (!p || p >= tmp_tail)
				break;

			++depth;

			savep = ++p;
		}

		if (!depth) /* done */
			break;

		search_from = (save_close + 1);
		savep = search_from;

		while (depth > 0)
		{
			p = strstr(savep, close_pattern);

			if (!p || p >= tail)
				break;

			--depth;

			save_close = p;
			savep = ++p;
		}

		if (depth) /* error */
		{
			fprintf(stderr, "__extract_area: failed to find matching closing tag for \"%s\"\n", first_part.buf_head);
			goto fail;
		}

		buf_clear(&tmp_buf);
		buf_append_ex(&tmp_buf, search_from, (save_close - search_from));
		tmp_tail = tmp_buf.buf_tail;
		savep = tmp_buf.buf_head;
	}

	buf_clear(&tmp_buf);
	buf_append_ex(dbuf, save_start, (save_close - save_start));

	out:
	buf_destroy(&tmp_buf);
	buf_destroy(&first_part);
	return 0;

	fail:
	buf_destroy(&tmp_buf);
	buf_destroy(&first_part);
	return -1;
}

#if 0
static int
__remove_braces(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *p;
	char *savep;
	char *outer_start;
	char *outer_end;
	char *search_from;
	int depth = 0;
	size_t range;

/*
 * 1. FIND LEFT BRACE
 * 2. IF NO LEFT BRACE, GOTO 9, ELSE FIND NEXT RIGHT BRACE
 * 3. SEARCH [LEFTBRACE+1,RIGHT_BRACE) FOR MORE LEFT BRACES
 * 4. IF MORE LEFT BRACES, FIND EQUAL NUMBER OF RIGHT BRACES FROM [RIGHTBRACE+1...TAIL), ELSE GOTO 7
 * 5. SEARCH RANGE [RIGHTBRACE+1...NEW RIGHT BRACE) FOR MORE LEFT BRACES
 * 6. GOTO 4
 * 7. REMOVE RANGE [FIRST LEFT BRACE...LAST RIGHT BRACE]
 * 8. GO TO START OF BUFFER, GOTO 1
 * 9. END
 */

/*
 * TODO: Use the function __nested_closing_char() to get closing braces
 */
	while(1)
	{
		p = savep = buf->buf_head;
		depth = 0;

		outer_start = memchr(savep, 0x7b, (tail - savep));

		if (!outer_start)
			goto out;

		search_from = (outer_start + 1);
		outer_end = memchr(search_from, 0x7d, (tail - search_from));

		if (!outer_end)
		{
			outer_end = tail;
			goto out_collapse;
		}

		++outer_end;

		for(;;)
		{
			savep = search_from;
			depth = 0;

			while(1)
			{
				p = memchr(savep, 0x7b, (outer_end - savep));

				if (!p)
					break;

				++depth;

				savep = ++p;

				if (p >= outer_end)
					break;
			}

			if (depth == 0) /* then we've found the outermost right brace for our initial left brace */
			{
#ifdef DEBUG
				printf("removing \x1b[38;5;9mLINE_START\x1b[m%.*s\x1b[38;5;9mLINE_END\x1b[m\n", (int)(outer_end - outer_start), outer_start);
#endif
				buf_collapse(buf, (off_t)(outer_start - buf->buf_head), (outer_end - outer_start));
				break;
			}

			search_from = outer_end;
			savep = search_from;

			while (depth > 0)
			{
				if (*outer_end != 0x7d)
					outer_end = memchr(savep, 0x7d, (tail - savep));

				if (!outer_end)
				{
					outer_end = tail;
					goto out_collapse;
				}

				--depth;

				++outer_end;
				savep = outer_end;

				if (savep >= tail)
					break;
			}
		} /* for(;;) */
	} /* while(1) */


	/*
	 * We jump here if we never found a matching closing
	 * right brace, so we just clear from the outer
	 * left brace to the tail of the buffer.
	 */
	out_collapse:
	range = (outer_end - outer_start);
	buf_collapse(buf, (off_t)(outer_start - buf->buf_head), range);

	out:
	return 0;
}
#endif

static void
__normalise_file_title(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *p = buf->buf_head;

	p = buf->buf_head;
	while (p < tail)
	{
		if (*p == 0x20
		|| (*p != 0x5f && !isalpha(*p) && !isdigit(*p)))
		{
			*p++ = 0x5f;
			if (*(p-2) == 0x5f)
			{
				--p;
				buf_collapse(buf, (off_t)(p - buf->buf_head), (size_t)1);
				tail = buf->buf_tail;
			}

			continue;
		}

		++p;
	}

	while (!isalpha(*(buf->buf_tail - 1)) && !isdigit(*(buf->buf_tail - 1)))
		buf_snip(buf, 1);

	if (option_set(OPT_FORMAT_XML))
		buf_append(buf, ".xml");
	else
		buf_append(buf, ".txt");

	return;
}

static void
__remove_excess_nl(buf_t *buf)
{
	assert(buf);

	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	savep = buf->buf_head;

	while (1)
	{
		p = memchr(savep, 0x0a, (tail - savep));

		if (!p)
			break;

		savep = p;

		while (*p == 0x0a && p < tail)
			++p;

		range = (p - savep);

		if (range > 2)
		{
			savep += 2;

			range = (p - savep);

			buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
			p = savep;
			tail = buf->buf_tail;
		}

		savep = p;
	}

	return;
}

static void
__remove_excess_sp(buf_t *buf)
{
	assert(buf);

	char *p;
	char *savep;
	char *tail = buf->buf_tail;
	size_t range;

	savep = buf->buf_head;

	while (1)
	{
		p = memchr(savep, ' ', (tail - savep));

		if (!p || p >= tail)
			break;

		++p;

		savep = p;
		while (isspace(*p))
			++p;

		range = (p - savep);

		if (range)
		{
			buf_collapse(buf, (off_t)(savep - buf->buf_head), range);
			p = savep;
			tail = buf->buf_tail;
		}
	}

	return;
}

/**
 * __nested_closing_char - return a pointer to the final closing character
 * for example, the correct '}' char to go with its opening '{'
 * @whence: pointer to the opening char
 * @limit: pointer to our search limit
 * @o: the opening character
 * @c: the closing character
 */
char *
__nested_closing_char(char *whence, char *limit, char o, char c)
{
	assert(whence);
	assert(limit);
	assert(limit > whence);

	char *search_from;
	char *cur_pos;
	char *savep;
	char *final;
	int depth = 0;

	search_from = savep = (whence + 1);
	final = memchr(search_from, c, (limit - search_from));

	if (!final)
		return NULL;

	while (1)
	{
		savep = search_from;

		while (1)
		{
			cur_pos = memchr(savep, o, (final - savep));

			if (!cur_pos)
				break;

			++depth;

			if (cur_pos >= final)
				savep = cur_pos;
			else
				savep = (cur_pos + 1);
		}

		if (!depth)
			break;

		search_from = savep = (final + 1);

		while (depth)
		{
			assert(limit >= savep);
			cur_pos = memchr(savep, c, (limit - savep));

			if (!cur_pos)
				break;

			--depth;

			final = cur_pos;
			savep = (final + 1);
		}

		depth = 0;
	}

	assert(final);
	return final;
}

#if 0
/**
 * __get_outermost_closing - return a pointer to one byte past the closing pattern
 * @whence: position from which to start search
 * @opattern: the corresponding open pattern for the enclosing sequence
 * @cpattern: the closing pattern of which the outermost one is sought
 */
char *
__get_outermost_closing(char *whence, char *opattern, char *cpattern)
{
	char *p = whence;
	char *savep;
	char *search_from;
	char *close;
	int depth = 0;
	size_t oplen = strlen(opattern);
	size_t cplen = strlen(cpattern);

	search_from = p;
	savep = p;
	close = strstr(search_from, cpattern);

	if (!close)
		return NULL;

	close += cplen;

	while (1)
	{
		savep = search_from;

		while (1)
		{
			p = strstr(savep, opattern);
			if (p)
			{
				++depth;
				savep = (p + oplen);
			}
			else
			{
				break;
			}
		}

		if (!depth)
			break;

		search_from = savep = close;

		while (depth)
		{
			p = strstr(savep, cpattern);
			if (!p)
			{
				if (depth)
				{
					fprintf(stderr, "__get_outermost_closing: failed to find required closing pattern\n");
					return NULL;
				}

				break;
			}

			savep = (p + cplen);
			--depth;
		}

		close = savep;
	}

	return close;
}
#endif

static void
tex_replace_fractions(buf_t *buf)
{
	assert(buf);
}

static void
__replace_tex(buf_t *buf)
{
	assert(buf);

	buf_replace(buf, "\\displaystyle", "");
	buf_replace(buf, "\\forall", "∀");
	buf_replace(buf, "\\exists", "∃");
	buf_replace(buf, "\\leq", "<=");
	buf_replace(buf, "\\geq", ">=");
	buf_replace(buf, "\\epsilon", "ε");
	buf_replace(buf, "\\alpha", "α");
	buf_replace(buf, "\\Alpha", "Α");
	buf_replace(buf, "\\beta", "β");
	buf_replace(buf, "\\Beta", "Β");
	buf_replace(buf, "\\gamma", "γ");
	buf_replace(buf, "\\Gamma", "Γ");
	buf_replace(buf, "\\pi", "π");
	buf_replace(buf, "\\Pi", "Π");
	buf_replace(buf, "\\phi", "Φ");
	buf_replace(buf, "\\varphi", "φ");
	buf_replace(buf, "\\theta", "θ");
	buf_replace(buf, "\\cos", "cos");
	buf_replace(buf, "\\sin", "sin");
	buf_replace(buf, "\\tan", "tan");
	buf_replace(buf, "\\cot", "cot");
	buf_replace(buf, "\\sec", "sec");
	buf_replace(buf, "\\csc", "csc");
	buf_replace(buf, "\\infty", "∞");
	buf_replace(buf, "\\in", " \xe2\x88\x88");
	buf_replace(buf, "\\backslash", " \\ ");
	buf_replace(buf, "\\colon", ":");
	buf_replace(buf, "\\varphi", "ϕ");
	buf_replace(buf, "\\Rightarrow", "→");
	buf_replace(buf, "\\quad", " ");
	buf_replace(buf, "&=", "=");

	tex_replace_fractions(buf);

	return;
}

int
__parse_maths_expressions(buf_t *buf)
{
	assert(buf);

	char *exp_start;
	char *exp_end;
	char *savep;
	char *tail = buf->buf_tail;
	buf_t tmp;
	size_t elen;
	size_t tlen;
	off_t sp_off;

	savep = buf->buf_head;
	buf_init(&tmp, 1024);

	while (1)
	{
		exp_start = strstr(savep, "{\\displaystyle");

		if (!exp_start || exp_start >= tail)
			break;

		exp_end = __nested_closing_char(exp_start, buf->buf_tail, '{', '}');

		if (!exp_end)
			break;

		elen = (exp_end - exp_start);

		buf_append_ex(&tmp, exp_start, (exp_end - exp_start));
		*(tmp.buf_tail) = 0;

		__replace_tex(&tmp);

		tlen = tmp.data_len;
		if (elen > tlen)
		{
			strncpy(exp_start, tmp.buf_head, tlen);
			exp_start += tlen;
			buf_collapse(buf, (off_t)(exp_start - buf->buf_head), (elen - tlen));
			tail = buf->buf_tail;
		}
		else
		{
			sp_off = (off_t)(savep - buf->buf_head);
			buf_shift(buf, (off_t)(exp_start - buf->buf_head), (tlen - elen));
			savep = (buf->buf_head + sp_off);
			exp_start = savep;
			tail = buf->buf_tail;
			strncpy(exp_start, tmp.buf_head, tlen);
			exp_start += tlen;
		}

		savep = exp_start;
		buf_clear(&tmp);
	}

	buf_destroy(&tmp);
	return 0;
}

int
extract_wiki_article(buf_t *buf)
{
	int out_fd = -1;
	buf_t file_title;
	buf_t content_buf;
	http_header_t *server;
	http_header_t *date;
	http_header_t *lastmod;
	static char inet6_string[INET6_ADDRSTRLEN];
	char *buffer = NULL;
	char *home;
	struct sockaddr_in sock4;
	struct sockaddr_in6 sock6;
	struct addrinfo *ainf = NULL;
	struct addrinfo *aip = NULL;
	int gotv4 = 0;
	int gotv6 = 0;
	wiki_cache_t *value_cache = NULL;
	wiki_cache_t *content_cache = NULL;
	struct article_header article_header;
	size_t vlen;

	if (!(buffer = calloc(DEFAULT_TMP_BUF_SIZE, 1)))
		goto fail;

	clear_struct(&article_header);

	/*
	 * Extract data for display
	 * at top of the article.
	 */
	value_cache = wiki_cache_create(
			"value_cache",
			sizeof(value_t),
			0,
			value_cache_ctor,
			value_cache_dtor);

	content_cache = wiki_cache_create(
			"content_cache",
			sizeof(content_t),
			0,
			content_cache_ctor,
			content_cache_dtor);

	article_header.title = (value_t *)wiki_cache_alloc(value_cache, &article_header.title);
	article_header.server_name = (value_t *)wiki_cache_alloc(value_cache, &article_header.server_name);
	article_header.server_ipv4 = (value_t *)wiki_cache_alloc(value_cache, &article_header.server_ipv4);
	article_header.server_ipv6 = (value_t *)wiki_cache_alloc(value_cache, &article_header.server_ipv6);
	article_header.generator = (value_t *)wiki_cache_alloc(value_cache, &article_header.generator);
	article_header.lastmod = (value_t *)wiki_cache_alloc(value_cache, &article_header.lastmod);
	article_header.downloaded = (value_t *)wiki_cache_alloc(value_cache, &article_header.downloaded);
	article_header.content_len = (value_t *)wiki_cache_alloc(value_cache, &article_header.content_len);

	/* Extract these values from the HTTP response header */
	server = (http_header_t *)wiki_cache_alloc(http_hcache, &server);
	date = (http_header_t *)wiki_cache_alloc(http_hcache, &date);
	lastmod = (http_header_t *)wiki_cache_alloc(http_hcache, &lastmod);

	http_fetch_header(buf, "Server", server, (off_t)0);
	http_fetch_header(buf, "Date", date, (off_t)0);
	http_fetch_header(buf, "Last-Modified", lastmod, (off_t)0);

	strcpy(article_header.server_name->value, server->value);
	article_header.server_name->vlen = server->vlen;

	strcpy(article_header.downloaded->value, date->value);
	article_header.downloaded->vlen = date->vlen;

	strcpy(article_header.lastmod->value, lastmod->value);
	article_header.lastmod->vlen = lastmod->vlen;

	wiki_cache_dealloc(http_hcache, (void *)server, &server);
	wiki_cache_dealloc(http_hcache, (void *)date, &date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod, &lastmod);

	buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE);
	buf_init(&file_title, pathconf("/", _PC_PATH_MAX));

	home = getenv("HOME");
	buf_append(&file_title, home);
	buf_append(&file_title, WIKIGRAB_DIR);
	buf_append(&file_title, "/");

	__get_tag_content(buf, "<title");

	buf_append(&content_buf, tag_content);
	__normalise_file_title(&content_buf);

	buf_append(&file_title, content_buf.buf_head);
	buf_clear(&content_buf);

	vlen = strlen(tag_content);
	strncpy(article_header.title->value, tag_content, vlen);
	article_header.title->value[vlen] = 0;
	article_header.title->vlen = vlen;

	/*
	 * Replace spaces (and any non-underscore non-ascii chars) with underscores.
	 */
	__get_tag_field(buf, "<meta name=\"generator\"", "content");

	vlen = strlen(tag_content);
	strncpy(article_header.generator->value, tag_content, vlen);
	article_header.generator->value[vlen] = 0;
	article_header.generator->vlen = vlen;

	if ((out_fd = open(file_title.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) < 0)
		goto out_destroy_bufs;

	fprintf(stdout, "Created file \"%s\"\n", file_title.buf_head);

	if (server->value[0])
	{
		clear_struct(&sock4);
		clear_struct(&sock6);

		getaddrinfo(server->value, NULL, NULL, &ainf);
		if (ainf)
		{
			for (aip = ainf; aip; aip = aip->ai_next)
			{
				if (aip->ai_socktype == SOCK_STREAM
				&& aip->ai_family == AF_INET)
				{
					memcpy(&sock4, aip->ai_addr, aip->ai_addrlen);
					gotv4 = 1;
					break;
				}
			}

			for (aip = ainf; aip; aip = aip->ai_next)
			{
				if (aip->ai_family == AF_INET6)
				{
					memcpy(&sock6, aip->ai_addr, aip->ai_addrlen);
					gotv6 = 1;
					break;
				}
			}
		}
	}

	if (gotv4)
		strcpy(article_header.server_ipv4->value, inet_ntoa(sock4.sin_addr));
	else
		strcpy(article_header.server_ipv4->value, "None");

	if (gotv6)
		strcpy(article_header.server_ipv6->value, inet_ntop(AF_INET6, sock6.sin6_addr.s6_addr, inet6_string, INET6_ADDRSTRLEN));
	else
		strcpy(article_header.server_ipv6->value, "None");

/*
 * BEGIN PARSING THE TEXT FROM THE ARTICLE.
 */

	if (__extract_area(buf, &content_buf, "<div id=\"mw-content-text\"", "</div") < 0)
		goto out_destroy_file;

	__get_all(content_cache, &content_buf, "<p", "</p");
	__get_all(content_cache, &content_buf, "<li", "</li");
	__get_all(content_cache, &content_buf, "<math", "</math");
	__get_all(content_cache, &content_buf, "<table", "</table");

	buf_clear(&content_buf);

	/*
	 * Sort all the <p>,<ul>,<table>, etc based on offsets
	 * from the start of the extracted area data.
	 */
	qsort((void *)content_cache->cache,
				(size_t)wiki_cache_nr_used(content_cache),
				content_cache->objsize,
				sort_content_cache);

	int i;
	int nr_used = wiki_cache_nr_used(content_cache);
	content_t *cp;

	cp = (content_t *)content_cache->cache;
	for (i = 0; i < nr_used; ++i)
	{
		buf_append_ex(&content_buf, cp->data, cp->data_len);
		buf_append(&content_buf, "\n");
		++cp;
	}

	__parse_maths_expressions(&content_buf);

#if 0
	/*
	 * Mark certain HTML tags so we
	 * can correctly format the text file.
	 */
	if (option_set(OPT_FORMAT_TXT))
	{
		__mark_list_tags(&content_buf);
		__mark_table_tags(&content_buf);
	}
#endif

	__remove_html_tags(&content_buf);
	__remove_inline_refs(&content_buf);
	__remove_html_encodings(&content_buf);
	__replace_html_entities(&content_buf);
	//__remove_garbage(&content_buf);
	//__remove_braces(&content_buf);
	__remove_excess_nl(&content_buf);
	__remove_excess_sp(&content_buf);

	/*
 	 * List marks for .txt format will be dealt with
	 * in __do_format_txt()
	 */

	if (option_set(OPT_FORMAT_XML))
	{
		buf_append(&content_buf, "</text>\n</wiki>\n");
		sprintf(article_header.content_len->value, "%lu", content_buf.data_len);
		article_header.content_len->vlen = strlen(article_header.content_len->value);

		sprintf(buffer,
			"%s\n"
			"<wiki>\n"
			"\t<metadata>\n"
			"\t\t<meta name=\"Title\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Parser\" content=\"WikiGrab v%s\"/>\n"
			"\t\t<meta name=\"Server\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Server-ipv4\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Server-ipv6\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Generator\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Modified\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Downloaded\" content=\"%s\"/>\n"
			"\t\t<meta name=\"Length\" content=\"%s\"/>\n"
			"\t</metadata>\n"
			"\t<text>\n",
			XML_START_LINE,
			article_header.title->value,
			WIKIGRAB_BUILD,
			article_header.server_name->value,
			article_header.server_ipv4->value,
			article_header.server_ipv6->value,
			article_header.generator->value,
			article_header.lastmod->value,
			article_header.downloaded->value,
			article_header.content_len->value);
	}
	else
	{
		__do_format_txt(&content_buf);

		sprintf(article_header.content_len->value, "%lu", content_buf.data_len);
		article_header.content_len->vlen = strlen(article_header.content_len->value);
		
		sprintf(buffer,
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n"
			"                    Downloaded via WikiGrab v%s\n\n"
			"      >>>>>>>>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n"
			"%*s%s\n\n"
			"      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n\n",
			WIKIGRAB_BUILD,
			LEFT_ALIGN_WIDTH, "Title: ", article_header.title->value,
			LEFT_ALIGN_WIDTH, "Served-by: ", article_header.server_name->value,
			LEFT_ALIGN_WIDTH, "Generated-by: ", article_header.generator->value,
			LEFT_ALIGN_WIDTH, "Server-ip-v4: ", article_header.server_ipv4->value,
			LEFT_ALIGN_WIDTH, "Server-ip-v6: ", article_header.server_ipv6->value,
			LEFT_ALIGN_WIDTH, "Last-modified: ", article_header.lastmod->value,
			LEFT_ALIGN_WIDTH, "Downloaded: ", article_header.downloaded->value,
			LEFT_ALIGN_WIDTH, "Content-length: ", article_header.content_len->value);
	}

	write(out_fd, buffer, strlen(buffer));
	buf_write_fd(out_fd, &content_buf);
	close(out_fd);
	out_fd = -1;

	if (option_set(OPT_OPEN_FINISH))
	{
		pid_t child;

		child = fork();

		if (!child)
		{
			execlp("gedit", file_title.buf_head, (char *)0);
		}
	}

	wiki_cache_dealloc(value_cache, (void *)article_header.title, &article_header.title);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_name, &article_header.server_name);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv4, &article_header.server_ipv4);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv6, &article_header.server_ipv6);
	wiki_cache_dealloc(value_cache, (void *)article_header.generator, &article_header.generator);
	wiki_cache_dealloc(value_cache, (void *)article_header.content_len, &article_header.content_len);
	wiki_cache_dealloc(value_cache, (void *)article_header.lastmod, &article_header.lastmod);
	wiki_cache_dealloc(value_cache, (void *)article_header.downloaded, &article_header.downloaded);

	wiki_cache_destroy(value_cache);
	wiki_cache_destroy(content_cache);

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	free(buffer);
	buffer = NULL;

	return 0;

	out_destroy_file:

	ftruncate(out_fd, (off_t)0);
	unlink(file_title.buf_head);

	out_destroy_bufs:

	buf_destroy(&content_buf);
	buf_destroy(&file_title);

	free(buffer);
	buffer = NULL;

	wiki_cache_dealloc(value_cache, (void *)article_header.title, &article_header.title);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_name, &article_header.server_name);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv4, &article_header.server_ipv4);
	wiki_cache_dealloc(value_cache, (void *)article_header.server_ipv6, &article_header.server_ipv6);
	wiki_cache_dealloc(value_cache, (void *)article_header.generator, &article_header.generator);
	wiki_cache_dealloc(value_cache, (void *)article_header.content_len, &article_header.content_len);
	wiki_cache_dealloc(value_cache, (void *)article_header.lastmod, &article_header.lastmod);
	wiki_cache_dealloc(value_cache, (void *)article_header.downloaded, &article_header.downloaded);

	wiki_cache_destroy(value_cache);
	wiki_cache_destroy(content_cache);

	fail:
	return -1;
}
