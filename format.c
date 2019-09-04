#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "buffer.h"
#include "format.h"
#include "wikigrab.h"

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

		if (!p)
			break;

		*p++ = 0x22;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 5);
		savep = p;
		tail -= 5;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&lt;");

		if (!p)
			break;

		*p++ = 0x3c;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 3);
		savep = p;
		tail -= 3;
	}

	p = savep = buf->buf_head;
	while(1)
	{
		p = strstr(savep, "&gt;");

		if (!p)
			break;

		*p++ = 0x3e;
		buf_collapse(buf, (off_t)(p - buf->buf_head), 3);
		savep = p;
		tail -= 3;
	}

	return;
}

int
format_article(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *line_start;
	char *line_end;
	char *p;
	char *savep;
	char *left;
	char *right;
	char *new_line;
	size_t line_len;
	size_t delta;
	int	gaps = 0;
	int passes;
	int remainder;
	int volte_face = 0;

	p = buf->buf_head;
	if (*p != 0x0a)
	{
		buf_shift(buf, (off_t)0, 3);
		strncpy(p, "\n\n\n", 3);
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

		while (1)
		{
			if (savep >= line_end)
				break;

			p = memchr(savep, 0x0a, (line_end - savep));

			if (!p)
				break;

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
			while (*line_end != 0x20 && line_end > (line_start + 1))
				--line_end;

			if (line_end == line_start)
			{
				line_end += WIKI_ARTICLE_LINE_LENGTH;
				buf_shift(buf, (off_t)(line_end - buf->buf_head), 1);
				++tail;
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
		{
			line_end = new_line = tail;
		}

		line_len = (new_line - line_start);
		delta = (WIKI_ARTICLE_LINE_LENGTH - line_len);

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

				savep = p;

				if (savep >= new_line)
					break;
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

				buf_shift(buf, (off_t)(p - buf->buf_head), 1);
				++line_end;
				++new_line;
				++tail;

				*p++ = 0x20;

				while (*p == 0x20)
					++p;

				savep = p;

				if (savep >= new_line)
					break;
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
							right = new_line;
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
							right = new_line;
							volte_face = 0;
							continue;
						}
					}

					buf_shift(buf, (off_t)(p - buf->buf_head), 1);

					++line_end;
					++new_line;
					++right;
					++tail;

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
