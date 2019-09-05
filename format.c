#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "buffer.h"
#include "format.h"
#include "wikigrab.h"

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

static void
replace_html_entities(buf_t *buf)
{
	char *tail = buf->buf_tail;
	char *p;
	char *savep;

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&quot;");

		if (!p || p >= tail)
			break;

		*p++ = 0x22;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 5);
		savep = p;
		tail = buf->buf_tail;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&lt;");

		if (!p || p >= tail)
			break;

		*p++ = 0x3c;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 3);
		savep = p;
		tail = buf->buf_tail;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&gt;");

		if (!p || p >= tail)
			break;

		*p++ = 0x3e;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 3);
		savep = p;
		tail = buf->buf_tail;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&amp;");

		if (!p || p >= tail)
			break;

		*p++ = '&';
		buf_collapse(buf, (off_t)(p - buf->buf_head), 4);
		savep = p;
		tail = buf->buf_tail;
	}

	return;
}

int
format_article(buf_t *buf)
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

	p = buf->buf_head;
	if (*p == 0x0a)
	{
		while (*p == 0x0a && p < tail)
			++p;

		buf_collapse(buf, (off_t)0, (p - buf->buf_head));
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

	replace_html_entities(buf);

	return 0;
}
