#include <assert.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "html.h"
#include "http.h"
#include "parse.h"
#include "tex.h"
#include "types.h"
#include "utils.h"
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
	fprintf(stderr,
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
	fprintf(stderr,
		"freeing content_t object:\n"
		"ptr: %p\n"
		"data: %s\n"
		"len: %lu bytes\n"
		"off: %lu bytes\n",
		content->data,
		content->data,
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
			|| !strncmp(p, "<pre", 4))
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
			|| !strncmp("</pre", p, 5))
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
			|| !strncmp("</pre", p, 5))
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

/*
 * buf_shift() could require extending the buffer,
 * and this in turn could result in our data
 * being copied to somewhere else in the heap.
 * So save all the offsets and restore them
 * after the shift.
 */
#define BUF_SHIFT_SAFE(by, optr)\
do {\
	size_t __lstart_off = 0;\
	size_t __lend_off = 0;\
	size_t __nloff = 0;\
	size_t __rightoff = 0;\
	size_t __poff = 0;\
	size_t __spoff = 0;\
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
__count_non_ascii(char *data, char *end)
{
	char *p = data;
	int count = 0;

	while (p < end)
	{
		if (!isascii(*p))
		{
			if ((*p >> 4) == 0x0e)
				count += 2;
			else
				++count;
		}

		++p;
	}

	return count;
}

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
	int nr_nascii;
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
		nr_nascii = __count_non_ascii(line_start, new_line);
		if (nr_nascii > 0)
			line_len -= (nr_nascii >> 1);

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

	if (buf_init(&tmp_buf, DEFAULT_TMP_BUF_SIZE) < 0)
		goto fail;

	if (buf_init(&first_part, 32) < 0)
		goto fail_release_bufs;

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

	fail_release_bufs:
	buf_destroy(&tmp_buf);
	buf_destroy(&first_part);

	fail:
	return -1;
}

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
		|| (*p != 0x5f && !isalpha(*p) && !isdigit(*p) && isascii(*p)))
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

	p = (buf->buf_tail - 1);
	while (!isalpha(*p) && !isdigit(*p))
		--p;

	++p;

	if ((buf->buf_tail - p) > 0)
		buf_snip(buf, (buf->buf_tail - p));

	if (option_set(OPT_FORMAT_XML))
		buf_append(buf, ".xml");
	else
		buf_append(buf, ".txt");

	return;
}

static int
parse_maths_expressions(buf_t *buf)
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
	off_t off;

	savep = buf->buf_head;

	if (buf_init(&tmp, 1024) < 0)
		goto fail;

	while (1)
	{
		exp_start = strstr(savep, "{\\displaystyle");

		if (!exp_start || exp_start >= tail)
			break;

		exp_end = nested_closing_char(exp_start, buf->buf_tail, '{', '}');

		if (!exp_end)
			break;

		elen = (exp_end - exp_start);

		buf_append_ex(&tmp, exp_start+1, ((exp_end - exp_start) - 1));
		*(tmp.buf_tail) = 0;

		if (tex_replace_symbols(&tmp) < 0)
		{
			fprintf(stderr, "parse_maths_expressions: tex_replace_symbols error\n");
			goto fail;
		}

		tlen = tmp.data_len;
		if (elen > tlen)
		{
			memcpy(exp_start, tmp.buf_head, tlen);
			exp_start += tlen;
			buf_collapse(buf, (off_t)(exp_start - buf->buf_head), (elen - tlen));
			tail = buf->buf_tail;
		}
		else
		{
			sp_off = (off_t)(savep - buf->buf_head);
			off = (off_t)(exp_start - buf->buf_head);
			buf_shift(buf, off, (tlen - elen));
			exp_start = (buf->buf_head + off);
			memcpy(exp_start, tmp.buf_head, tlen);
			exp_start += tlen;
			savep = (buf->buf_head + sp_off);
			tail = buf->buf_tail;
		}

		savep = exp_start;
		buf_clear(&tmp);
	}

	buf_destroy(&tmp);
	return 0;

	fail:
	return -1;
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
	int i;
	wiki_cache_t *value_cache = NULL;
	wiki_cache_t *content_cache = NULL;
	struct article_header article_header;
	size_t vlen;
	size_t len;
	char *tag_content_ptr;

	fprintf(stdout, "Parsing Wikipedia article\n");

	if (!(buffer = calloc(DEFAULT_TMP_BUF_SIZE, 1)))
	{
		fprintf(stderr, "extract_wiki_article: failed to allocate memory for buffer (%s)\n", strerror(errno));
		goto fail;
	}

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

	if (!http_fetch_header(buf, "Server", server, (off_t)0))
		goto out_destroy_file;
	if (!http_fetch_header(buf, "Date", date, (off_t)0))
		goto out_destroy_file;
	if (!http_fetch_header(buf, "Last-Modified", lastmod, (off_t)0))
		goto out_destroy_file;

	if (server->vlen < MAX_VALUE_LEN)
		len = server->vlen;
	else
		len = (MAX_VALUE_LEN - 1);

	strncpy(article_header.server_name->value, server->value, len);
	article_header.server_name->value[len] = 0;
	article_header.server_name->vlen = len;

	if (date->vlen < MAX_VALUE_LEN)
		len = date->vlen;
	else
		len = (MAX_VALUE_LEN - 1);

	strncpy(article_header.downloaded->value, date->value, len);
	article_header.downloaded->value[len] = 0;
	article_header.downloaded->vlen = len;

	if (lastmod->vlen < MAX_VALUE_LEN)
		len = lastmod->vlen;
	else
		len = (MAX_VALUE_LEN - 1);

	strncpy(article_header.lastmod->value, lastmod->value, len);
	article_header.lastmod->value[len] = 0;
	article_header.lastmod->vlen = lastmod->vlen;

	wiki_cache_dealloc(http_hcache, (void *)server, &server);
	wiki_cache_dealloc(http_hcache, (void *)date, &date);
	wiki_cache_dealloc(http_hcache, (void *)lastmod, &lastmod);

	if (buf_init(&content_buf, DEFAULT_TMP_BUF_SIZE) < 0)
		goto fail_release_mem;

	if (buf_init(&file_title, pathconf("/", _PC_PATH_MAX)) < 0)
		goto fail_release_mem;

	home = getenv("HOME");
	buf_append(&file_title, home);
	buf_append(&file_title, WIKIGRAB_DIR);
	buf_append(&file_title, "/");

	tag_content_ptr = html_get_tag_content(buf, "<title");

	char *_p;

	if ((_p = strstr(tag_content_ptr, " - Wiki")))
		*_p = 0;

	buf_append(&content_buf, tag_content_ptr);
	__normalise_file_title(&content_buf);

	buf_append(&file_title, content_buf.buf_head);
	buf_clear(&content_buf);

	vlen = strlen(tag_content_ptr);

	if (vlen < MAX_VALUE_LEN)
		len = vlen;
	else
		len = (MAX_VALUE_LEN - 1);

	strncpy(article_header.title->value, tag_content_ptr, len);
	article_header.title->value[len] = 0;
	article_header.title->vlen = len;

	tag_content_ptr = html_get_tag_field(buf, "<meta name=\"generator\"", "content");

	vlen = strlen(tag_content_ptr);

	if (vlen < MAX_VALUE_LEN)
		len = vlen;
	else
		len = (MAX_VALUE_LEN - 1);

	strncpy(article_header.generator->value, tag_content_ptr, len);
	article_header.generator->value[len] = 0;
	article_header.generator->vlen = len;

	if ((out_fd = open(file_title.buf_head, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR)) < 0)
		goto fail_release_mem;

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

	fprintf(stdout, "Formatting text\n");

	if (__extract_area(buf, &content_buf, "<div id=\"mw-content-text\"", "</div") < 0)
		goto out_destroy_file;

/* Stuff we do not want */
	const char *const unwanted_class[] =
	{
		"box-Multiple_issues",
		"mw-references-wrap",
		"toc",
		"mw-empty-elt",
		"mw-editsection",
		"citation",
		"infobox",
		"navbox",
		"box-Cleanup",
		"box-Expand_language",
		"hatnote",
		"vertical-navbox",
		"gallery",
		NULL
	};

	const char *const unwanted_id[] =
	{
		"cite_note-FOOTNOTE",
		"See_also",
		"Notes",
		"References",
		"External_links",
		"coordinates",
		NULL
	};

	for (i = 0; unwanted_class[i] != NULL; ++i)
	{
		if (html_remove_elements_class(&content_buf, unwanted_class[i]) < 0)
			goto out_destroy_file;
	}

	for (i = 0; unwanted_id[i] != NULL; ++i)
	{
		if (html_remove_elements_id(&content_buf, unwanted_id[i]) < 0)
			goto out_destroy_file;
	}

	html_remove_content(&content_buf, "<style", "</style");

#if 0
	html_remove_elements_class(&content_buf, "box-Multiple_issues");
	html_remove_elements_class(&content_buf, "mw-references-wrap");
	html_remove_elements_class(&content_buf, "toc");
	html_remove_elements_class(&content_buf, "mw-empty-elt");
	html_remove_elements_class(&content_buf, "mw-editsection");
	html_remove_elements_class(&content_buf, "citation");
	html_remove_elements_class(&content_buf, "infobox");
	html_remove_elements_class(&content_buf, "navbox");
	html_remove_elements_class(&content_buf, "box-Cleanup");
	html_remove_elements_class(&content_buf, "box-Expand_language");
	html_remove_elements_class(&content_buf, "hatnote");
	html_remove_elements_class(&content_buf, "vertical-navbox");
	html_remove_elements_class(&content_buf, "gallery");
	html_remove_elements_id(&content_buf, "cite_note-FOOTNOTE");
	html_remove_elements_id(&content_buf, "See_also");
	html_remove_elements_id(&content_buf, "Notes");
	html_remove_elements_id(&content_buf, "References");
	html_remove_elements_id(&content_buf, "External_links");
#endif

/* Stuff we want */
	if (html_get_all(content_cache, &content_buf, "<p", "</p") < 0)
		goto out_destroy_file;

	if (html_get_all(content_cache, &content_buf, "<dl>", "</dl>") < 0)
		goto out_destroy_file;

	if (html_get_all(content_cache, &content_buf, "<pre", "</pre") < 0)
		goto out_destroy_file;

/*
 * If we put "<li", it will parse "<link" and possibly match it with </li>.
 * So we have to put <li>. In any case, it would seem that the only <li>
 * content that is of the format "<li class=..." is that related to
 * citations, references, etc, which we do not want anyway. The ones that
 * are formatted as <li> in articles seems to be what we are after.
 */
	if (html_get_all(content_cache, &content_buf, "<li>", "</li>") < 0)
		goto out_destroy_file;

/*
 * Use this instead of taking <math> tags because we can end up with several
 * duplicate equations sometimes due to one being here and one also being
 * elsewhere (not bounded in <>)
 */

	int nr_maths = 0;
	if ((nr_maths = html_get_all(content_cache, &content_buf, "<annotation encoding=\"application/x-tex\"", "</annotation")) < 0)
		goto out_destroy_file;

	if (html_get_all_class(content_cache, &content_buf, "mw-headline") < 0)
		goto out_destroy_file;

	//if (html_get_all(content_cache, &content_buf, "<table", "</table") < 0)
		//goto out_destroy_file;

/*
 * Now sort the extracted content by offset from start of buffer.
 */
	qsort((void *)content_cache->cache,
				(size_t)wiki_cache_nr_used(content_cache),
				content_cache->objsize,
				sort_content_cache);

	int nr_used = wiki_cache_nr_used(content_cache);
	content_t *cp = (content_t *)content_cache->cache;
	buf_clear(&content_buf);

	for (i = 0; i < nr_used; ++i)
	{
		buf_append_ex(&content_buf, cp->data, cp->data_len);
		buf_append(&content_buf, "\n\n");
		++cp;
	}

	if (nr_maths > 0)
		parse_maths_expressions(&content_buf);

	__remove_html_tags(&content_buf);
	__remove_inline_refs(&content_buf);
	__remove_html_encodings(&content_buf);
	__replace_html_entities(&content_buf);
	remove_excess_nl(&content_buf);
	remove_excess_sp(&content_buf);


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

		int title_offset = ((WIKI_ARTICLE_LINE_LENGTH - article_header.title->vlen) / 2);
		int title_width = (title_offset + (int)article_header.title->vlen);
		static char wgb[64];

		sprintf(wgb, "WikiGrab v%s", WIKIGRAB_BUILD);
		
		sprintf(buffer,
			"\n%*s\n\n"
			"  {\n"
			"    \"server\" : \"%s\",\n"
			"    \"v4-addr\" : \"%s\",\n"
			"    \"v6-addr\" : \"%s\",\n"
			"    \"last-modified\" : \"%s\",\n"
			"    \"generator\" : \"%s\",\n"
			"    \"content-length\" : \"%s bytes\",\n"
			"    \"downloaded\" : \"%s\"\n"
			"  }\n"
			"\n\n"
			"%*s\n\n\n",
			WIKI_ARTICLE_LINE_LENGTH, wgb,
			article_header.server_name->value,
			article_header.server_ipv4->value,
			article_header.server_ipv6->value,
			article_header.lastmod->value,
			article_header.generator->value,
			article_header.content_len->value,
			article_header.downloaded->value,
			title_width, article_header.title->value);
	}

/*
 * Remove trailing new lines at end of article.
 */
	if (*(content_buf.buf_tail - 1) == 0x0a)
	{
		char *t = content_buf.buf_tail - 1;
		while (*t == 0x0a)
			--t;
		++t;
		buf_snip(&content_buf, (content_buf.buf_tail - t));
	}

	fprintf(stdout, "Writing to file\n");

	size_t buf_len = strlen(buffer);
	if (write(out_fd, buffer, buf_len) != buf_len) /* Our article header */
	{
		fprintf(stderr, "extract_wiki_article: failed to write to file (%s)\n", strerror(errno));
		goto out_destroy_file;
	}

	buf_write_fd(out_fd, &content_buf); /* The article */
	close(out_fd);
	out_fd = -1;

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

	fprintf(stdout, "Finished!\n");

	return 0;

	out_destroy_file:

	if (ftruncate(out_fd, (off_t)0) < 0)
		;
	unlink(file_title.buf_head);

	fail_release_mem:
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
