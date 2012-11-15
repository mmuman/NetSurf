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

/*
 * docs:
 * gopher://gopher.floodgap.com/1/gopher/tech
 * gopher://gopher.floodgap.com/0/overbite/dbrowse?pluginm%201
 *
 * tests:
 * gopher://sdf.org/1/sdf/historical	images
 * gopher://gopher.r-36.net/1/	large photos
 * gopher://sdf.org/1/sdf/classes	binaries
 * gopher://sdf.org/1/users/	long page
 * gopher://jgw.mdns.org/1/	search items
 * gopher://jgw.mdns.org/1/MISC/	's' item (sound)
 * gopher://gopher.floodgap.com/1/gopher	broken link - fixed 2012/04/08
 * gopher://sdf.org/1/maps/m	missing lines - fixed 2012/04/08
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <sys/param.h>

#include <libwapcaplet/libwapcaplet.h>
#include "content/content_protected.h"
#include "content/fetch.h"
#include "content/gopher.h"
#include "desktop/gui.h"
#include "desktop/options.h"
#include "utils/http.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/url.h"

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
	struct gopher_state *s;
	s = malloc(sizeof(struct gopher_state));
	if (s == NULL)
		return NULL;

	s->url = nsurl_ref(url);
	s->fetch_handle = fetch_handle;
	s->head_done = false;
	s->cached = 0;
	s->input = NULL;

	s->type = GOPHER_TYPE_NONE;
	url_gopher_type(nsurl_access(url), &s->type);
	/* on error s->type is left unchanged */

	return s;
}

/**
 * Finalise the state object.
 */

void gopher_state_free(struct gopher_state *s)
{
	free(s->input);
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
	char buffer[4096];
	const char *p = data;
	size_t left = size;
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

	LOG(("iteration: cached %d left %d", s->cached, left));

	if (s->cached) {
		s->input = realloc(s->input, s->cached + left);
		memcpy(s->input + s->cached, data, left);
		p = s->input;
		left += s->cached;
		s->cached = left;
	}

	LOG(("copied: cached %d left %d", s->cached, left));

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
	if (left) {
		if (!s->input)
			s->input = malloc(left);
		memmove(s->input, p, left);
	}
	s->cached = left;
	/* no need to realloc, either we will again right away or free. */

	return size;

}

/**
 * Return an HTTP code for the gopher connection to the cURL fetcher.
 *
 * \return	HTTP code
 */

long gopher_get_http_code(struct gopher_state *s, char *data, size_t size)
{
	if (s->type == GOPHER_TYPE_NONE)
		/* it's really a bad request */
		return 400;

	if (size == 0)
		/* delay until we get data */
		return 0;

	/* We didn't receive anything yet, check for error.
	 * type '3' items report an error
	 */
	/*LOG(("data[0] == 0x%02x '%c'", data[0], data[0]));*/
	if (data[0] == GOPHER_TYPE_ERROR) {
		lwc_string *path;
		size_t i = 1;

		if (gopher_need_generate(s->type)) {
			/* TODO: try to guess better from the string ?
			 * like "3 '/bcd' doesn't exist!"
			 * XXX: it might not always be a 404
			 */
			return 404;
		}
		/* Check more carefully for possible error vs valid data.
		 * Usually we get something like:
		 * 3 '/foo' does not exist	error.host	1
		 */
		if (i >= size)
			return 200;
		if (data[i] == ' ')
			i++;
		if (i >= size)
			return 200;
		if (data[i++] != '\'')
			return 200;
		path = nsurl_get_component(s->url, NSURL_PATH);
		if (path == NULL)
			return 200;
		if (lwc_string_length(path) < 2 ||
				(size - i) < lwc_string_length(path) ||
				strncmp(&data[i], lwc_string_data(path) + 2,
				lwc_string_length(path) - 2)) {
			lwc_string_unref(path);
			return 200;
		}
		i += lwc_string_length(path) - 2;
		lwc_string_unref(path);
		if (i >= size)
			return 200;
		if (data[i++] != '\'')
			return 200;
		/* XXX: check even more? */

		s->type = GOPHER_TYPE_DIRECTORY;
		/* force the Content-type */
		gopher_probe_mime(s, NULL, 0);

		return 404;
	}

	return 200;
}

/**
 * Probe the MIME type for the gopher handle, and send Content-type header.
 *
 * \return	true iff MIME type was correctly guessed.
 */

bool gopher_probe_mime(struct gopher_state *s, char *data, size_t size)
{
	const char *mime;
	char h[80];
	fetch_msg msg;

	mime = gopher_type_to_mime(s->type);

	/* leave other types unknown and let the mime sniffer handle them */

	if (mime) {
		LOG(("gopher %p mime is '%s'", s, mime));
		snprintf(h, sizeof h, "Content-type: %s\r\n", mime);
		h[sizeof h - 1] = 0;

		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t *) h;
		msg.data.header_or_data.len = strlen(h);
		fetch_send_callback(&msg, s->fetch_handle);

		return true;
	}

	LOG(("gopher %p unknown mime (type '%c')", s, s->type));

	return false;
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
			"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n"
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
			"<div class=\"uplink dontprint\">\n"
			"<a href=\"..\">[up]</a>\n"
			"<a href=\"/\">[top]</a>\n"
			"</div>\n"
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

#define HTML_LF "\n"

	switch (type) {
	case GOPHER_TYPE_ENDOFPAGE:
		/* end of the page */
		*buffer = '\0';
		break;
	case GOPHER_TYPE_TEXTPLAIN:
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"text\">%s</span></a>"
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
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"binary\">%s</span></a>"
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
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"dir\">%s</span></a>"
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
				"<form method=\"get\" action=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"query\">"
				"<label>%s "
				"<input name=\"\" type=\"text\" align=\"right\" />"
				"</label>"
				"</span></form>"
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
				"<a href=\"telnet://%s%s%s%s%s\">"
				"<span class=\"telnet\">%s</span></a>"
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
				"<a href=\"tn3270://%s%s%s%s%s\">"
				"<span class=\"telnet\">%s</span></a>"
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
				"<a href=\"cso://%s%s%s\">"
				"<span class=\"cso\">%s</span></a>"
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
					"<a href=\"gopher://%s%s%s/%c%s\">"
					"<span class=\"img\">%s" /* </span><br/> */
					/*"<span class=\"img\" >"*/
					"<img src=\"gopher://%s%s%s/%c%s\" alt=\"%s\"/>"
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
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"img\">%s</span></a>"
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
					"<a href=\"%s\">"
					"<span class=\"html\">%s</span></a>"
					"<br/>"HTML_LF,
					redirect_url,
					nice_text);
		} else {
			/* cf. gopher://sdf.org/1/sdf/classes/ */
			error = snprintf(buffer, buffer_length,
					"<a href=\"gopher://%s%s%s/%c%s\">"
					"<span class=\"html\">%s</span></a>"
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
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"audio\">%s</span></a>"
				"<audio src=\"gopher://%s%s%s/%c%s\" controls=\"controls\">"
				"<span>[player]</span></audio>"
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
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"other\">%s</span></a>"
				"<br/>"HTML_LF,
				fields[FIELD_HOST],
				alt_port ? ":" : "",
				alt_port ? fields[FIELD_PORT] : "",
				type, fields[FIELD_SELECTOR], nice_text);
		break;
	case GOPHER_TYPE_MOVIE:
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"video\">%s</span></a>"
				"<video src=\"gopher://%s%s%s/%c%s\" controls=\"controls\">"
				"<span>[player]</span></video>"
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
	default:
		/* yet to be tested items, please report when you see them! */
		LOG(("warning: unknown gopher item type 0x%02x '%c'", type, type));
		error = snprintf(buffer, buffer_length,
				"<a href=\"gopher://%s%s%s/%c%s\">"
				"<span class=\"unknown\">%s</span></a>"
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
				/* force reparsing the type */
				type = 0;
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
			if (sz < 2)
				continue;	/* \n should be in the next buffer */
			if (p[1] != '\n') {
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

	/* unfinished item, cleanup */
	for (i = FIELD_NAME; i < FIELD_COUNT; i++) {
		free(fields[i]);
		fields[i] = NULL;
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


