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

/** Types of fields in a line */
typedef enum {
	FIELD_NAME,
	FIELD_SELECTOR,
	FIELD_HOST,
	FIELD_PORT,
	FIELD_GPFLAG,
	FIELD_EOL,
	FIELD_COUNT = FIELD_EOL
} gopher_field;

/** Map of gopher types to MIME types */
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
	{ GOPHER_TYPE_PDF_ALT, "application/pdf" },
	{ GOPHER_TYPE_PDF, "application/pdf" },
	{ GOPHER_TYPE_PNG, "image/png"},
	{ 0, NULL }
};

/**
 * Initialise the state object.
 */

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

/**
 * Finalise the state object.
 */

void gopher_state_free(struct gopher_state *s)
{
	nsurl_unref(s->url);
	free(s);
}

/**
 * Handle incoming data from the fetcher and convert it to HTML.
 *
 * \return  the amount of consumed data
 *
 * This calls of a series of functions.  To generate a complete page,
 * call the following functions in order:
 *
 *     gopher_generate_top()
 *     gopher_generate_title()
 *     gopher_generate_row()           -- call 'n' times for 'n' rows
 *     gopher_generate_bottom()
 *
 */

size_t gopher_fetch_data(struct gopher_state *s, char *data, size_t size)
{
	char buffer[1024];
	const char *p = data;
	size_t remaining = size;
	size_t left;
	fetch_msg msg;
	LOG(("gopher %p, (,, %d)", s, size));

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

	/* XXX: should we loop there until remaining is 0 ? Seems not needed. */
	/* XXX: note there is a possibility of endless loop if a line is larger
	 * than the input buffer...
	 */

	LOG(("iteration: cached %d remaining %d", s->cached, remaining));

	p = s->input;
	left = MIN(sizeof(s->input) - s->cached, remaining);
	memcpy(s->input + s->cached, data, left);
	remaining -= left;
	data += left;
	s->cached += left;
	left = s->cached;

	LOG(("copied: cached %d remaining %d", s->cached, remaining));

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
		/* XXX: should we implement
		 * gopher://gophernicus.org/0/doc/gopher/gopher-title-resource.txt ?
		 */
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
		LOG(("done row, left %d", left));
		/* send data to the caller */
		/*LOG(("FETCH_DATA"));*/
		msg.type = FETCH_DATA;
		msg.data.header_or_data.buf = (const uint8_t *) buffer;
		msg.data.header_or_data.len = strlen(buffer);
		fetch_send_callback(&msg, s->fetch_handle);
	}
	LOG(("last row, left %d", left));

	/* move the remainder to the beginning of the buffer */
	if (left)
		memmove(s->input, s->input + s->cached - left, left);
	s->cached = left;

	return size;

}

/**
 * Escape a string using HTML entities.
 *
 * \return	malloc()ed string.
 */

static char *html_escape_string(const char *str)
{
	char *nice_str, *cnv;
	const char *tmp;

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

/**
 * Generate a title based on the directory path.
 *
 * \return	malloc()ed string.
 */

static char *gen_nice_title(const char *path)
{
	char *nice_path;
	char *title;
	int title_length;

	/* Convert path for display */
	nice_path = html_escape_string(path);
	if (nice_path == NULL) {
		return NULL;
	}

	/* Construct a localised title string */
	title_length = strlen(nice_path) + strlen(messages_get("FileIndex"));
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
 */

const char *gopher_type_to_mime(gopher_item_type type)
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

bool gopher_need_generate(gopher_item_type type)
{
	switch (type) {
	case GOPHER_TYPE_DIRECTORY:
	case GOPHER_TYPE_QUERY:
		return true;
	default:
		return false;
	}
}


/**
 * Generates the top part of an HTML directory listing page
 *
 * \return  true iff buffer filled without error
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
			"<link rel=\"stylesheet\" title=\"Standard\" "
				"type=\"text/css\" href=\"resource:internal.css\">\n"
			"<link rel=\"icon\" type=\"image/png\" href=\"resource:icons/directory.png\">\n");

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

static bool gopher_generate_row_internal(char type, char *fields[FIELD_COUNT],
		char *buffer, int buffer_length)
{
	char *nice_text;
	char *redirect_url = NULL;
	int error = 0;
	bool alt_port = false;
	char *username = NULL;

	if (fields[FIELD_PORT] && fields[FIELD_PORT][0]
			&& strcmp(fields[FIELD_PORT], "70"))
		alt_port = true;

	/* escape html special characters */
	nice_text = html_escape_string(fields[FIELD_NAME]);

	/* XXX: outputting \n generates better looking html code,
	 * but currently screws up indentation due to a bug.
	 */
#define HTML_LF 
/*#define HTML_LF "\n"*/

	switch (type) {
	case GOPHER_TYPE_ENDOFPAGE:
		/* end of the page */
		*buffer = '\0';
		break;
	case GOPHER_TYPE_TEXTPLAIN:
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"text\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_BINARY:
	case GOPHER_TYPE_BINHEX:
	case GOPHER_TYPE_BINARCHIVE:
	case GOPHER_TYPE_UUENCODED:
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"binary\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_DIRECTORY:
		/*
		 * directory link
		 */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"dir\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_ERROR:
		error = snprintf(buffer, buffer_length,
				"<span class=\"error\">%s</span><br/>"HTML_LF,
				nice_text);
		break;
	case GOPHER_TYPE_QUERY:
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
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_TELNET:
		/* telnet: links
		 * cf. gopher://78.80.30.202/1/ps3
		 * -> gopher://78.80.30.202:23/8/ps3/new -> new@78.80.30.202
		 */
		alt_port = false;
		if (fields[FIELD_PORT] && fields[FIELD_PORT][0] &&
				strcmp(fields[FIELD_PORT], "23"))
			alt_port = true;
		username = strrchr(fields[FIELD_SELECTOR], '/');
		if (username)
			username++;
		error = snprintf(buffer, buffer_length,
				"<a href=\"telnet://%s%s%s%s%s\">"HTML_LF
				"<span class=\"telnet\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				username ? username : "",
				username ? "@" : "",
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				nice_text);
		break;
	case GOPHER_TYPE_TN3270:
		/* tn3270: URI scheme, cf. http://tools.ietf.org/html/rfc6270 */
		alt_port = false;
		if (fields[FIELD_PORT] && fields[FIELD_PORT][0] &&
				strcmp(fields[FIELD_PORT], "23"))
			alt_port = true;
		username = strrchr(fields[FIELD_SELECTOR], '/');
		if (username)
			username++;
		error = snprintf(buffer, buffer_length,
				"<a href=\"tn3270://%s%s%s%s%s\">"HTML_LF
				"<span class=\"telnet\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				username ? username : "",
				username ? "@" : "",
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				nice_text);
		break;
	case GOPHER_TYPE_CSO_SEARCH:
		/* CSO search.
		 * At least Lynx supports a cso:// URI scheme:
		 * http://lynx.isc.org/lynx2.8.5/lynx2-8-5/lynx_help/lynx_url_support.html
		 */
		alt_port = false;
		if (fields[FIELD_PORT] && fields[FIELD_PORT][0] &&
				strcmp(fields[FIELD_PORT], "105"))
			alt_port = true;
		error = snprintf(buffer, buffer_length,
				"<a href=\"cso://%s%s%s\">"HTML_LF
				"<span class=\"cso\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				nice_text);
		break;
	case GOPHER_TYPE_GIF:
	case GOPHER_TYPE_IMAGE:
	case GOPHER_TYPE_PNG:
	case GOPHER_TYPE_BITMAP:
		/* quite dangerous, cf. gopher://namcub.accela-labs.com/1/pics */
		if (nsoption_bool(gopher_inline_images)) {
			error = snprintf(buffer, buffer_length,
					"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
					"<span class=\"img\">%s"HTML_LF /* </span><br/> */
					/*"<span class=\"img\" >"HTML_LF*/
					"<img src=\"gopher://%s%s%s/%c%s\" alt=\"%s\"/>"HTML_LF
					"</span>"
					"</a>"
					"<br/>"HTML_LF,
					fields[FIELD_HOST],
					alt_port ? ":" : "",
					alt_port ? fields[FIELD_PORT] : "",
					type, fields[FIELD_SELECTOR],
					nice_text,
					fields[FIELD_HOST],
					alt_port ? ":" : "",
					alt_port ? fields[FIELD_PORT] : "",
					type, fields[FIELD_SELECTOR],
					nice_text);
			break;
		}
		/* fallback to default, link them */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"img\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_HTML:
		if (fields[FIELD_SELECTOR] &&
				strncmp(fields[FIELD_SELECTOR], "URL:", SLEN("URL:")) == 0)
			redirect_url = fields[FIELD_SELECTOR] + SLEN("URL:");

		/* cf. gopher://pineapple.vg/1 */
		if (fields[FIELD_SELECTOR] &&
				strncmp(fields[FIELD_SELECTOR], "/URL:", SLEN("/URL:")) == 0)
			redirect_url = fields[FIELD_SELECTOR] + SLEN("/URL:");

		if (redirect_url) {
			error = snprintf(buffer, buffer_length,
					"<a href=\"%s\">"HTML_LF
					"<span class=\"html\">%s</span></a>"HTML_LF
					"<br/>"HTML_LF,
					redirect_url,
					nice_text);
		} else {
			/* cf. gopher://sdf.org/1/sdf/classes/ */
			error = snprintf(buffer, buffer_length,
					"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
					"<span class=\"html\">%s</span></a>"HTML_LF
					"<br/>"HTML_LF,
					fields[FIELD_HOST],
					alt_port ? ":" : "",
					alt_port ? fields[FIELD_PORT] : "",
					type, fields[FIELD_SELECTOR], nice_text);
		}
		break;
	case GOPHER_TYPE_INFO:
		if ((fields[FIELD_SELECTOR] &&
				strcmp(fields[FIELD_SELECTOR], "TITLE")) == 0) {
			error = snprintf(buffer, buffer_length,
					"<h2>%s</h2><br/>"HTML_LF,
					nice_text);
		} else {
			error = snprintf(buffer, buffer_length,
					"<span class=\"info\">%s</span><br/>"HTML_LF,
					nice_text);
		}
		break;
	case GOPHER_TYPE_AUDIO:
	case GOPHER_TYPE_SOUND:
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"audio\">%s</span></a>"HTML_LF
				"<audio src=\"gopher://%s%s%s/%c%s\" controls=\"controls\">"
				"<span>[player]</span></audio>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR]);
		break;
	case GOPHER_TYPE_PDF:
	case GOPHER_TYPE_PDF_ALT:
		/* generic case for known-to-work items */
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"other\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_MOVIE:
		/* TODO */
		/* FALLTHROUGH */
	default:
		/* yet to be tested items, please report when you see them! */
		LOG(("warning: unknown gopher item type 0x%02x '%c'", type, type));
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"HTML_LF
				"<span class=\"unknown\">%s</span></a>"HTML_LF
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
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
 */

static bool gopher_generate_row(const char **data, size_t *size,
		char *buffer, int buffer_length)
{
	bool ok = false;
	char type = 0;
	int field = 0;
	/* name, selector, host, port, gopher+ flag */
	char *fields[FIELD_COUNT] = { NULL, NULL, NULL, NULL, NULL };
	const char *s = *data;
	const char *p = *data;
	int i;
	size_t sz = *size;

	for (; sz && *p; p++, sz--) {
		if (!type) {
			type = *p;
			if (!type || type == '\n' || type == '\r') {
				LOG(("warning: invalid gopher item type 0x%02x", type));
			}
			s++;
			continue;
		}
		switch (*p) {
		case '\n':
			if (field > 0) {
				LOG(("warning: unterminated gopher item '%c'", type));
			}
			/* FALLTHROUGH */
		case '\r':
			if (sz < 1 || p[1] != '\n') {
				LOG(("warning: CR without LF in gopher item '%c'", type));
			}
			if (field < FIELD_PORT && type != '.') {
				LOG(("warning: unterminated gopher item '%c'", type));
			}
			fields[field] = malloc(p - s + 1);
			memcpy(fields[field], s, p - s);
			fields[field][p - s] = '\0';
			if (type == '.' && field == 0 && p == s) {
				;/* XXX: signal end of page? For now we just ignore it. */
			}
			ok = gopher_generate_row_internal(type, fields, buffer, buffer_length);
			for (i = FIELD_NAME; i < FIELD_COUNT; i++) {
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
			if (field >= FIELD_GPFLAG) {
				LOG(("warning: extra tab in gopher item '%c'", type));
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


