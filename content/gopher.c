/*
 * Copyright 2011 François Revol <mmu_man@users.sourceforge.net>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
 * Generate HTML content for displaying gopher directory listings (implementation).
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <sys/param.h>

#include "content/content_protected.h"
#include "content/fetch.h"
#include "content/gopher.h"
#include "desktop/gui.h"
#include "desktop/options.h"
#include "utils/http.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"

static char *gen_nice_title(const char *path);
static bool gopher_generate_top(char *buffer, int buffer_length);
static bool gopher_generate_title(const char *title, char *buffer,
		int buffer_length);
static bool gopher_generate_row(const char **data, size_t *size,
		char *buffer, int buffer_length);
static bool gopher_generate_bottom(char *buffer, int buffer_length);

static struct {
	gopher_item_type type;
	const char *mime;
} gopher_type_map[] = {
	/* these come from http://tools.ietf.org/html/rfc1436 */
	{ GOPHER_TYPE_TEXTPLAIN, "text/plain" },
	{ GOPHER_TYPE_DIRECTORY, "text/html;charset=UTF-8" },
	{ GOPHER_TYPE_QUERY, "text/html;charset=UTF-8" },
	{ GOPHER_TYPE_GIF, "image/gif" },
	{ GOPHER_TYPE_HTML, "text/html" },
	/* those are not standardized */
	{ GOPHER_TYPE_PDF, "application/pdf" },
	{ GOPHER_TYPE_PNG, "image/png"},
	{ 0, NULL }
};

struct gopher_state *gopher_state_create(nsurl *url, struct fetch *fetch_handle)
{
	struct gopher_state *ctx;
	ctx = malloc(sizeof(struct gopher_state));
	if (ctx == NULL)
		return NULL;
	/**/
	ctx->url = nsurl_ref(url);
	ctx->fetch_handle = fetch_handle;
	ctx->head_done = false;
	ctx->cached = 0;
	return ctx;
}

void gopher_state_free(struct gopher_state *s)
{
	nsurl_unref(s->url);
	free(s);
}


size_t gopher_fetch_data(struct gopher_state *s, char *data, size_t size)
{
	char buffer[1024];
	char *p = data;
	size_t remaining = size;
	size_t left;
	fetch_msg msg;
	fprintf(stderr, "%s(,, %d)\n", __FUNCTION__, size);

	/* actually called when done getting it all */
	if (size == 0)
	{
		if (s->type && gopher_need_generate(s->type)) {
			if (gopher_generate_bottom(buffer, sizeof(buffer)))
			{
				/* send data to the caller */
				/*LOG(("FETCH_DATA"));*/
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = (const uint8_t *) buffer;
				msg.data.header_or_data.len = strlen(buffer);
				fetch_send_callback(&msg, s->fetch_handle);
			}
		}
		return 0;
	}

	fprintf(stderr, "iteration: cached %d remaining %d\n", s->cached, remaining);

	p = s->input;
	left = MIN(sizeof(s->input) - s->cached, remaining);
	memcpy(s->input + s->cached, data, left);
	remaining -= left;
	data += left;
	s->cached += left;
	left = s->cached;

	fprintf(stderr, "copied: cached %d remaining %d\n", s->cached, remaining);

	if (!s->head_done)
	{
		char *title;
		if (gopher_generate_top(buffer, sizeof(buffer)))
		{
			/* send data to the caller */
			/*LOG(("FETCH_DATA"));*/
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf = (const uint8_t *) buffer;
			msg.data.header_or_data.len = strlen(buffer);
			fetch_send_callback(&msg, s->fetch_handle);
		}
		title = gen_nice_title(nsurl_access(s->url));
		if (gopher_generate_title(title, buffer, sizeof(buffer)))
		{
			/* send data to the caller */
			/*LOG(("FETCH_DATA"));*/
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf = (const uint8_t *) buffer;
			msg.data.header_or_data.len = strlen(buffer);
			fetch_send_callback(&msg, s->fetch_handle);
		}
		free(title);
		s->head_done = true;
	}

	while (gopher_generate_row(&p, &left, buffer, sizeof(buffer)))
	{
		//fprintf(stderr, "done row, left %d\n", left);
		/* send data to the caller */
		/*LOG(("FETCH_DATA"));*/
		msg.type = FETCH_DATA;
		msg.data.header_or_data.buf = (const uint8_t *) buffer;
		msg.data.header_or_data.len = strlen(buffer);
		fetch_send_callback(&msg, s->fetch_handle);
	}
	fprintf(stderr, "last row, left %d\n", left);

	/* move the remainder to the beginning of the buffer */
	if (left)
		memmove(s->input, s->input + s->cached - left, left);
	s->cached = left;

	return size /* - left*/;

}


static char *html_escape_string(char *str)
{
	char *nice_str, *cnv, *tmp;

	if (str == NULL) {
		return NULL;
	}

	/* Convert str for display */
	nice_str = malloc(strlen(str) * SLEN("&amp;") + 1);
	if (nice_str == NULL) {
		return NULL;
	}

	/* Escape special HTML characters */
	for (cnv = nice_str, tmp = str; *tmp != '\0'; tmp++) {
		if (*tmp == '<') {
			*cnv++ = '&';
			*cnv++ = 'l';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '>') {
			*cnv++ = '&';
			*cnv++ = 'g';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '&') {
			*cnv++ = '&';
			*cnv++ = 'a';
			*cnv++ = 'm';
			*cnv++ = 'p';
			*cnv++ = ';';
		} else {
			*cnv++ = *tmp;
		}
	}
	*cnv = '\0';

	return nice_str;
}


static char *gen_nice_title(const char *path)
{
	const char *tmp;
	char *nice_path, *cnv;
	char *title;
	int title_length;

	/* Convert path for display */
	nice_path = malloc(strlen(path) * SLEN("&amp;") + 1);
	if (nice_path == NULL) {
		return NULL;
	}

	/* Escape special HTML characters */
	for (cnv = nice_path, tmp = path; *tmp != '\0'; tmp++) {
		if (*tmp == '<') {
			*cnv++ = '&';
			*cnv++ = 'l';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '>') {
			*cnv++ = '&';
			*cnv++ = 'g';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '&') {
			*cnv++ = '&';
			*cnv++ = 'a';
			*cnv++ = 'm';
			*cnv++ = 'p';
			*cnv++ = ';';
		} else {
			*cnv++ = *tmp;
		}
	}
	*cnv = '\0';

	/* Construct a localised title string */
	title_length = (cnv - nice_path) + strlen(messages_get("FileIndex"));
	title = malloc(title_length + 1);

	if (title == NULL) {
		free(nice_path);
		return NULL;
	}

	/* Set title to localised "Index of <nice_path>" */
	snprintf(title, title_length, messages_get("FileIndex"), nice_path);

	free(nice_path);

	return title;
}


/**
 * Convert the gopher item type to mime type
 *
 * \return  MIME type string
 *
 */

const char *gopher_type_to_mime(char type)
{
	int i;

	for (i = 0; gopher_type_map[i].type; i++)
		if (gopher_type_map[i].type == type)
			return gopher_type_map[i].mime;
	return NULL;
}


/**
 * Tells if the gopher item type needs to be converted to html
 *
 * \return  true iff the item must be converted
 *
 */

bool gopher_need_generate(char type)
{
	switch (type) {
	case '1':
	case '7':
		return true;
	default:
		return false;
	}
}


/**
 * Generates the top part of an HTML directory listing page
 *
 * \return  true iff buffer filled without error
 *
 * This is part of a series of functions.  To generate a complete page,
 * call the following functions in order:
 *
 *     gopher_generate_top()
 *     gopher_generate_title()
 *     gopher_generate_row()           -- call 'n' times for 'n' rows
 *     gopher_generate_bottom()
 */

static bool gopher_generate_top(char *buffer, int buffer_length)
{
	int error = snprintf(buffer, buffer_length,
			"<html>\n"
			"<head>\n"
			/*"<!-- base href=\"%s\" -->\n"*//* XXX: needs the content url */
			/* Don't do that:
			 * seems to trigger a reparsing of the gopher data itself as html...
			 * "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
			 */
			/* TODO: move this to clean CSS in internal.css */
			"<link rel=\"stylesheet\" title=\"Standard\" "
				"type=\"text/css\" href=\"resource:internal.css\">\n");

	if (error < 0 || error >= buffer_length)
		/* Error or buffer too small */
		return false;
	else
		/* OK */
		return true;
}


/**
 * Generates the part of an HTML directory listing page that contains the title
 *
 * \param  title	  title to use
 * \param  buffer	  buffer to fill with generated HTML
 * \param  buffer_length  maximum size of buffer
 * \return  true iff buffer filled without error
 *
 * This is part of a series of functions.  To generate a complete page,
 * call the following functions in order:
 *
 *     gopher_generate_top()
 *     gopher_generate_title()
 *     gopher_generate_row()           -- call 'n' times for 'n' rows
 *     gopher_generate_bottom()
 */

static bool gopher_generate_title(const char *title, char *buffer, int buffer_length)
{
	int error;

	if (title == NULL)
		title = "";

	error = snprintf(buffer, buffer_length,
			"<title>%s</title>\n"
			"</head>\n"
			"<body id=\"gopher\">\n"
			"<h1>%s</h1>\n",
			title, title);
	if (error < 0 || error >= buffer_length)
		/* Error or buffer too small */
		return false;
	else
		/* OK */
		return true;
}

/**
 * Internal worker called by gopher_generate_row().
 */

static bool gopher_generate_row_internal(char type, char *fields[5],
		char *buffer, int buffer_length)
{
	char *nice_text;
	char *redirect_url = NULL;
	int error;
	bool alt_port = false;
	char *username = NULL;

	if (fields[3] && strcmp(fields[3], "70"))
		alt_port = true;

	/* escape html special characters */
	nice_text = html_escape_string(fields[0]);

	/* XXX: outputting \n generates better looking html code,
	 * but currently screws up indentation due to a bug.
	 */
#define HTML_LF 
/*#define HTML_LF "\n"*/

	switch (type) {
	case '.':
		/* end of the page */
		*buffer = '\0';
		break;
	case '0':	/* text/plain link */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"text\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	case '9':	/* binary */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"binary\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	case '1':
		/*
		 * directory link
		 */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"dir\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	case '3':
		/* Error
		 */
		error = snprintf(buffer, buffer_length,
				"<span class=\"error\">%s</span><br/>"HTML_LF,
				nice_text);
		break;
	case '7':
		/* TODO: handle search better.
		 * For now we use an unnamed input field and accept sending ?=foo
		 * as it seems at least Veronica-2 ignores the = but it's unclean.
		 */
		error = snprintf(buffer, buffer_length,
				"<form method=\"get\" action=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"query\">"
				"<label>%s "
				"<input name=\"\" type=\"text\" align=\"right\" />"
				"</label>"
				"</span></form>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	case '8':
		/* telnet: links
		 * cf. gopher://78.80.30.202/1/ps3
		 * -> gopher://78.80.30.202:23/8/ps3/new -> new@78.80.30.202
		 */
		alt_port = false;
		if (fields[3] && strcmp(fields[3], "23"))
			alt_port = true;
		username = strrchr(fields[1], '/');
		if (username)
			username++;
		error = snprintf(buffer, buffer_length,
				"<a href=\"telnet://%s%s%s%s%s\">"HTML_LF
				"<span class=\"dir\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				username ? username : "",
				username ? "@" : "",
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				nice_text);
		break;
	case 'g':
	case 'I':
	case 'p':
		/* quite dangerous, cf. gopher://namcub.accela-labs.com/1/pics */
		if (nsoption_bool(gopher_inline_images)) {
			error = snprintf(buffer, buffer_length,
					"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
					"<span class=\"img\">%s "HTML_LF /* </span><br/> */
					//"<span class=\"img\" >"HTML_LF
					"<img src=\"gopher://%s%s%s/%c%s\" alt=\"%s\"/>"HTML_LF
					"</span>"
					"</a>"
					"<br/>"HTML_LF,
					fields[2],
					alt_port ? ":" : "",
					alt_port ? fields[3] : "",
					type, fields[1],
					nice_text,
					fields[2],
					alt_port ? ":" : "",
					alt_port ? fields[3] : "",
					type, fields[1],
					nice_text);
			break;
		}
		/* fallback to default, link them */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"dir\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	case 'h':
		if (fields[1] && strncmp(fields[1], "URL:", 4) == 0)
			redirect_url = fields[1] + 4;
		/* cf. gopher://pineapple.vg/1 */
		if (fields[1] && strncmp(fields[1], "/URL:", 5) == 0)
			redirect_url = fields[1] + 5;
		if (redirect_url) {
			error = snprintf(buffer, buffer_length,
					"<a href=\"%s\">"HTML_LF
					"<span class=\"link\">%s</span></a>"HTML_LF
					"<br/>"HTML_LF,
					redirect_url,
					nice_text);
		} else {
			/* cf. gopher://sdf.org/1/sdf/classes/ */
			error = snprintf(buffer, buffer_length,
					"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
					"<span class=\"dir\">%s</span></a>"HTML_LF
					"<br/>"HTML_LF,
					fields[2],
					alt_port ? ":" : "",
					alt_port ? fields[3] : "",
					type, fields[1], nice_text);
		}
		break;
	case 'i':
		error = snprintf(buffer, buffer_length,
				"<span class=\"info\">%s</span><br/>"HTML_LF,
				nice_text);
		break;
	default:
		LOG(("warning: unknown gopher item type 0x%02x '%c'\n", type, type));
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"dir\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[2],
				alt_port ? ":" : "",
				alt_port ? fields[3] : "",
				type, fields[1], nice_text);
		break;
	}

	free(nice_text);

	if (error < 0 || error >= buffer_length)
		/* Error or buffer too small */
		return false;
	else
		/* OK */
		return true;
}


/**
 * Generates the part of an HTML directory listing page that displays a row
 * of the gopher data
 *
 * \param  size		  pointer to the data buffer pointer
 * \param  size		  pointer to the remaining data size
 * \param  buffer	  buffer to fill with generated HTML
 * \param  buffer_length  maximum size of buffer
 * \return  true iff buffer filled without error
 *
 * This is part of a series of functions.  To generate a complete page,
 * call the following functions in order:
 *
 *     gopher_generate_top()
 *     gopher_generate_title()
 *     gopher_generate_row()           -- call 'n' times for 'n' rows
 *     gopher_generate_bottom()
 */

static bool gopher_generate_row(const char **data, size_t *size,
		char *buffer, int buffer_length)
{
	bool ok = false;
	char type = 0;
	int field = 0;
	/* name, selector, host, port, gopher+ flag */
	char *fields[5] = { NULL, NULL, NULL, NULL, NULL };
	const char *s = *data;
	const char *p = *data;
	int i;
	size_t sz = *size;

	for (; sz && *p; p++, sz--) {
		if (!type) {
			type = *p;
			if (!type || type == '\n' || type == '\r') {
				LOG(("warning: invalid gopher item type 0x%02x\n", type));
			}
			s++;
			continue;
		}
		switch (*p) {
		case '\n':
			if (field > 0) {
				LOG(("warning: unterminated gopher item '%c'\n", type));
			}
			//FALLTHROUGH
		case '\r':
			if (sz < 1 || p[1] != '\n') {
				LOG(("warning: CR without LF in gopher item '%c'\n", type));
			}
			if (field < 3 && type != '.') {
				LOG(("warning: unterminated gopher item '%c'\n", type));
			}
			fields[field] = malloc(p - s + 1);
			memcpy(fields[field], s, p - s);
			fields[field][p - s] = '\0';
			if (type == '.' && field == 0 && p == s) {
				;/* XXX: signal end of page? For now we just ignore it. */
			}
			ok = gopher_generate_row_internal(type, fields, buffer, buffer_length);
			for (i = 0; i < 5; i++) {
				free(fields[i]);
				fields[i] = NULL;
			}
			sz--;
			p++;
			if (sz && *p == '\n') {
				p++;
				sz--;
			}
			*data = p;
			field = 0;
			if (ok)
				*size = sz;
			return ok;
		case '\x09':
			if (field >= 4) {
				LOG(("warning: extra tab in gopher item '%c'\n", type));
				break;
			}
			fields[field] = malloc(p - s + 1);
			memcpy(fields[field], s, p - s);
			fields[field][p - s] = '\0';
			field++;
			s = p + 1;
			break;
		default:
			break;
		}
	}

	return false;
}


/**
 * Generates the bottom part of an HTML directory listing page
 *
 * \return  Bottom of directory listing HTML
 *
 * This is part of a series of functions.  To generate a complete page,
 * call the following functions in order:
 *
 *     gopher_generate_top()
 *     gopher_generate_title()
 *     gopher_generate_row()           -- call 'n' times for 'n' rows
 *     gopher_generate_bottom()
 */

static bool gopher_generate_bottom(char *buffer, int buffer_length)
{
	int error = snprintf(buffer, buffer_length,
			"</div>\n"
			"</body>\n"
			"</html>\n");
	if (error < 0 || error >= buffer_length)
		/* Error or buffer too small */
		return false;
	else
		/* OK */
		return true;
}


