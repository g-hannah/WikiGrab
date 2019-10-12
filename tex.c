#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buffer.h"
#include "cache.h"
#include "tex.h"
#include "utils.h"
#include "wikigrab.h"

void
tex_replace_fractions(buf_t *buf)
{
	assert(buf);
	char *frac_start;
	char *frac_end;
	char numer[64];
	char denom[64];
	char *p;
	char *q;
	off_t off;
	buf_t tmp;
	size_t range;

	buf_init(&tmp, 128);

	frac_end = frac_start = buf->buf_head;

	while (1)
	{
		frac_start = strstr(frac_end, "{\\frac");

		if (!frac_start || frac_start >= buf->buf_tail)
			break;

		frac_end = nested_closing_char(frac_start, buf->buf_tail, '{', '}');

		if (!frac_end)
			break;

		p = memchr(frac_start + 1, '{', (frac_end - frac_start));
		if (!p)
			break;

		q = memchr(p, '}', (frac_end - p));

		if (!q)
			break;

		++p;

		strncpy(numer, p, (q - p));
		numer[q - p] = 0;

		p = ++q;

		if (*p != '{')
			p = memchr(q, '{', (frac_end - q));

		if (!p)
			break;

		q = memchr(p, '}', (frac_end - p));

		if (!q)
			break;

		++p;

		strncpy(denom, p, (q - p));
		denom[q - p] = 0;

		range = (frac_end - frac_start);
		buf_collapse(buf, (off_t)(frac_start - buf->buf_head), range);
		off = (off_t)(frac_start - buf->buf_head);
		buf_shift(buf, off, strlen(numer) + 2 + strlen(denom));
		frac_start = buf->buf_head + off;
		buf_append(&tmp, numer);
		buf_append(&tmp, "/");
		buf_append(&tmp, denom);
		buf_append(&tmp, " ");
		strncpy(frac_start, tmp.buf_head, tmp.data_len);

		buf_clear(&tmp);
	}

	buf_destroy(&tmp);
	return;
}

void
tex_replace_symbols(buf_t *buf)
{
	assert(buf);

	buf_replace(buf, "\\displaystyle", "");
	buf_replace(buf, "\\sum", "Σ");
	buf_replace(buf, "\\forall", "∀");
	buf_replace(buf, "\\exists", "∃");
	buf_replace(buf, "\\mapsto", "⟼");
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
	buf_replace(buf, "\\cong", "≅");
	buf_replace(buf, "\\cos", "cos");
	buf_replace(buf, "\\sin", "sin");
	buf_replace(buf, "\\tan", "tan");
	buf_replace(buf, "\\cot", "cot");
	buf_replace(buf, "\\sec", "sec");
	buf_replace(buf, "\\csc", "csc");
	buf_replace(buf, "\\infty", "∞");
	buf_replace(buf, "\\in", " ∈");
	buf_replace(buf, "\\notin", "∉");
	buf_replace(buf, "\\backslash", " \\ ");
	buf_replace(buf, "\\colon", ":");
	buf_replace(buf, "\\bar", " ̅");
	buf_replace(buf, "\\varphi", "ϕ");
	buf_replace(buf, "\\Rightarrow", "→");
	buf_replace(buf, "\\quad", " ");
	buf_replace(buf, "\\cdots", ". . .");
	buf_replace(buf, "\\vdots", "⋮");
	buf_replace(buf, "{\\begin{pmatrix}", "");
	buf_replace(buf, "\\end{pmatrix}", "");
	buf_replace(buf, "\\\\", "\n");
	buf_replace(buf, "&=", "=");

	tex_replace_fractions(buf);

	return;
}
