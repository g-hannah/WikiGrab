#include <assert.h>
#include <ctype.h>
#include <string.h>
#include "buffer.h"
#include "utils.h"

/**
 * nested_closing_char - return a pointer to the final closing character
 * for example, the correct '}' char to go with its opening '{'
 * @whence: pointer to the opening char
 * @limit: pointer to our search limit
 * @o: the opening character
 * @c: the closing character
 */
char *
nested_closing_char(char *whence, char *limit, char o, char c)
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

		if (savep >= limit)
			break;

		while (1)
		{
			cur_pos = memchr(savep, o, (final - savep));

			if (!cur_pos)
				break;

			++depth;

			if (cur_pos >= final)
				break;
			else
				savep = (cur_pos + 1);

			if (savep >= final)
				break;
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

			if (savep >= limit)
				break;
		}

		depth = 0;
		if (savep >= limit)
			break;
	}

	assert(final);
	return final;
}

void
remove_excess_sp(buf_t *buf)
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

void
remove_excess_nl(buf_t *buf)
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
