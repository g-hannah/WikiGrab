#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "buffer.h"
#include "format.h"
#include "wikigrab.h"

int
format_article(buf_t *buf)
{
	assert(buf);

	char *tail = buf->buf_tail;
	char *line_start;
	char *line_end;
	char *p;
	char *savep;

	line_start = buf->buf_head;

	while (1)
	{
		outer_loop_begin:
		line_end = (line_start + WIKI_ARTICLE_LINE_LENGTH);

		if (line_end > tail)
			line_end = tail;

		savep = line_start;

		while (1)
		{
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

		if (line_end == tail)
		{
			break;
		}
		else
		if (*line_end == 0x0a)
		{
			while (*line_end == 0x0a)
				++line_end;
		}
		else
		if (*line_end == 0x20)
		{
			*line_end++ = 0x0a;
		}
		else
		{
			while (*line_end != 0x20)
				--line_end;

			*line_end++ = 0x0a;
		}

		line_start = line_end;
	}

	return 0;
}
