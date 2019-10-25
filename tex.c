#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "cache.h"
#include "tex.h"
#include "utils.h"
#include "wikigrab.h"

static int
tex_replace_fractions(buf_t *buf)
{
	assert(buf);
	char *frac_start;
	char *frac_end;
	off_t off;
	buf_t tmp;
	size_t range;

	if (buf_init(&tmp, 128) < 0)
		goto fail;

	frac_end = frac_start = buf->buf_head;

	while (1)
	{
		frac_start = strstr(frac_end, "{\\frac");

		if (!frac_start || frac_start >= buf->buf_tail)
			break;

		frac_end = nested_closing_char(frac_start, buf->buf_tail, '{', '}');

		if (!frac_end)
			break;

		buf_append_ex(&tmp, frac_start, (frac_end - frac_start));
		BUF_NULL_TERMINATE(&tmp);

		buf_replace(&tmp, "}{", "/");
		buf_replace(&tmp, "{\\frac", "");
		buf_replace(&tmp, "{", "");
		buf_replace(&tmp, "}", "");

		range = (frac_end - frac_start) + 1;
		buf_collapse(buf, (off_t)(frac_start - buf->buf_head), range);
		off = (off_t)(frac_start - buf->buf_head);
		buf_shift(buf, off, tmp.data_len);
		frac_start = buf->buf_head + off;
		memcpy(frac_start, tmp.buf_head, tmp.data_len);

		buf_clear(&tmp);
	}

	buf_destroy(&tmp);
	return 0;

	fail:
	return -1;
}

static int
tex_replace_matrices(buf_t *buf)
{
	char *p;
	char *savep;
	char *end;
	buf_t tmp;

	buf_init(&tmp, 1024);
	savep = buf->buf_head;
#if 0

	while (1)
	{
		p = strstr(savep, "{\\begin{pmatrix}");

		if (!p || p >= buf->buf_tail)
			break;

		end = strstr(p, "{\\end{pmatrix}");

		if (!end)
			break;

		end += strlen("{\\end{pmatrix}");

		buf_append_ex(&tmp, p, (end - p));
		BUF_NULL_TERMINATE(&tmp);

		buf_collapse(buf, (off_t)(p - buf->buf_head), (end - p));

		buf_replace(&tmp, "{\\begin{pmatrix}", "");
		buf_replace(&tmp, "{\\end{pmatrix}", "");
	}
#endif

	return 0;
}

static int
tex_boldsymbol(buf_t *buf)
{
	assert(buf);

	buf_t tmp;
	char *p;
	char *savep;
	char *end_brace;
	char *symbol_start;
	char *symbol_end;
	off_t off;

	if (buf_init(&tmp, 256) < 0)
		goto fail;

	savep = buf->buf_head;

	while (1)
	{
		p = strstr(savep, "\\boldsymbol");

		if (!p || p >= buf->buf_tail)
			break;

		symbol_start = memchr(p, '{', (buf->buf_tail - p));
		if (!symbol_start)
			break;

		symbol_end = memchr(symbol_start, '}', (buf->buf_tail - symbol_start));

		if (!symbol_end)
			break;

		if (isspace(*(symbol_end - 1)))
			--symbol_end;

		++symbol_start;

		buf_append_ex(&tmp, symbol_start, (symbol_end - symbol_start));
		*(tmp.buf_tail) = 0;

		while (*p != '{' && p > (buf->buf_head + 1))
			--p;

		if (*p != '{')
			break;

		end_brace = nested_closing_char((p+1), buf->buf_tail, '{', '}');

		if (!end_brace)
			break;

		++end_brace;
		if (end_brace > buf->buf_tail)
			end_brace = buf->buf_tail;

		off = (off_t)(p - buf->buf_head);
		buf_collapse(buf, off, (end_brace - p));
		buf_shift(buf, off, tmp.data_len);
		p = (buf->buf_head + off);
		memcpy(p, tmp.buf_head, tmp.data_len);

		savep = ++p;
		buf_clear(&tmp);
	}

	buf_destroy(&tmp);
	return 0;

	fail:
	return -1;
}

struct tex_symbol_map
{
	char *tex;
	char *txt;
};

const struct tex_symbol_map symbol_map[] =
{
	{ "\\displaystyle", "" },
	{ "\\sum", "Σ" },
	{ "\\forall", "∀" },
	{ "\\exists", "∃" },
	{ "\\mapsto", "⟼" },
	{ "\\leq", "<=" },
	{ "\\geq", ">=" },
	{ "\\epsilon", "ε" },
	{ "\\alpha", "α" },
	{ "\\Alpha", "Α" },
	{ "\\beta", "β" },
	{ "\\Beta", "Β" },
	{ "\\gamma", "γ" },
	{ "\\Gamma", "Γ" },
	{ "\\pi", "π" },
	{ "\\Pi", "Π" },
	{ "\\phi", "Φ" },
	{ "\\varphi", "φ" },
	{ "\\theta", "θ" },
	{ "\\omega", "ω" },
	{ "\\Omega", "Ω" },
	{ "\\chi", "Χ" },
	{ "\\times", " ×" },
	{ "\\cong", " ≅" },
	{ "\\cos", "cos" },
	{ "\\sin", "sin" },
	{ "\\tan", "tan" },
	{ "\\cot", "cot" },
	{ "\\sec", "sec" },
	{ "\\csc", "csc" },
	{ "\\infty", "∞" },
	{ "\\in", " ∈ " },
	{ "\\notin", " ∉ " },
	{ "\\backslash", " \\ " },
	{ "\\colon", ":" },
	{ "\\bar", " ̅" },
	{ "\\varphi", "ϕ" },
	{ "\\Rightarrow", "→" },
	{ "\\quad", " " },
	{ "\\cdots", "..." },
	{ "\\vdots", "⋮" },
	{ "{\\begin{pmatrix}", "" },
	{ "\\end{pmatrix}", "" },
	{ "\\\\", "\n" },
	{ "&=", "=" },
	{ "\\left", "" },
	{ "\\right", "" },
	{ (char *)NULL, (char *)NULL }
};

int
tex_replace_symbols(buf_t *buf)
{
	assert(buf);

	int i;

	for (i = 0; symbol_map[i].tex && symbol_map[i].txt; ++i)
	{
		buf_replace(buf, symbol_map[i].tex, symbol_map[i].txt);
	}

	if (tex_replace_fractions(buf) < 0)
		goto fail;

	if (tex_replace_matrices(buf) < 0)
		goto fail;

	if (tex_boldsymbol(buf) < 0)
		goto fail;

	return 0;

	fail:
	return -1;
}
