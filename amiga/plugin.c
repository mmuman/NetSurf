/*
 * Copyright 2011 Chris Young <chris@unsatisfactorysoftware.co.uk>
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
 * Temporary "plugin" to pass unknown MIME types to DataTypes (implementation)
*/

#ifdef WITH_PLUGIN
#include "amiga/filetype.h"
#include "amiga/gui.h"
#include "amiga/plugin.h"
#include "content/content_protected.h"
#include "desktop/plotters.h"
#include "render/box.h"
#include "utils/log.h"
#include "utils/messages.h"

#include <proto/datatypes.h>
#include <proto/intuition.h>
#include <datatypes/pictureclass.h>

bool plugin_create(struct content *c, const struct http_parameter *params)
{
	LOG(("plugin_create"));

	return true;
}

bool plugin_convert(struct content *c)
{
	LOG(("plugin_convert"));

	union content_msg_data msg_data;
	int width, height;
	char title[100];
	const uint8 *data;
	UBYTE *bm_buffer;
	ULONG size;
	Object *dto;
	struct BitMapHeader *bmh;
	unsigned int bm_flags = BITMAP_NEW;
	int bm_format = PBPAFMT_RGBA;

	/* This is only relevant for picture datatypes... */

	data = (uint8 *)content__get_source_data(c, &size);

	if(c->data.plugin.dto = NewDTObject(NULL,
					DTA_SourceType, DTST_MEMORY,
					DTA_SourceAddress, data,
					DTA_SourceSize, size,
					DTA_GroupID, GID_PICTURE,
					PDTA_DestMode, PMODE_V43,
					TAG_DONE))
	{
		if(GetDTAttrs(c->data.plugin.dto, PDTA_BitMapHeader, &bmh, TAG_DONE))
		{
			width = (int)bmh->bmh_Width;
			height = (int)bmh->bmh_Height;

			c->bitmap = bitmap_create(width, height, bm_flags);
			if (!c->bitmap) {
				msg_data.error = messages_get("NoMemory");
				content_broadcast(c, CONTENT_MSG_ERROR, msg_data);
				return false;
			}

			bm_buffer = bitmap_get_buffer(c->bitmap);

			IDoMethod(c->data.plugin.dto, PDTM_READPIXELARRAY,
				bm_buffer, bm_format, bitmap_get_rowstride(c->bitmap),
				0, 0, width, height);
		}
		else return false;
	}
	else return false;

	c->width = width;
	c->height = height;

/*
	snprintf(title, sizeof(title), "image (%lux%lu, %lu bytes)",
		width, height, size);
	content__set_title(c, title);
*/

	bitmap_modified(c->bitmap);

	content_set_ready(c);
	content_set_done(c);

	content_set_status(c, "");
	return true;
}

void plugin_destroy(struct content *c)
{
	LOG(("plugin_destroy"));

	if (c->bitmap != NULL)
		bitmap_destroy(c->bitmap);

	DisposeDTObject(c->data.plugin.dto);

	return;
}

bool plugin_redraw(struct content *c, int x, int y,
	int width, int height, const struct rect *clip,
	float scale, colour background_colour)
{
	LOG(("plugin_redraw"));

	return plot.bitmap(x, y, width, height,
			c->bitmap, background_colour, BITMAPF_NONE);
}

/**
 * Handle a window containing a CONTENT_PLUGIN being opened.
 *
 * \param  c       content that has been opened
 * \param  bw      browser window containing the content
 * \param  page    content of type CONTENT_HTML containing c, or 0 if not an
 *                 object within a page
 * \param  box     box containing c, or 0 if not an object
 * \param  params  object parameters, or 0 if not an object
 */
void plugin_open(struct content *c, struct browser_window *bw,
	struct content *page, struct box *box,
	struct object_params *params)
{
	LOG(("plugin_open"));

	return;
}

void plugin_close(struct content *c)
{
	LOG(("plugin_close"));
	return;
}

void plugin_reformat(struct content *c, int width, int height)
{
	LOG(("plugin_reformat"));
	return;
}

bool plugin_clone(const struct content *old, struct content *new_content)
{
	LOG(("plugin_clone"));
	/* We "clone" the old content by replaying creation and conversion */
	if (plugin_create(new_content, NULL) == false)
		return false;

	if (old->status == CONTENT_STATUS_READY || 
			old->status == CONTENT_STATUS_DONE) {
		if (plugin_convert(new_content) == false)
			return false;
	}

	return true;
}

/**
 * Determines whether a content is handleable by a plugin
 *
 * \param mime_type The mime type of the content
 * \return true if the content is handleable, false otherwise
 */
bool plugin_handleable(const char *mime_type)
{
	LOG(("plugin_handleable %s", mime_type));

	char dt_mime[50];
	struct DataType *dt, *prevdt = NULL;
	bool found = false;

	while((dt = ObtainDataType(DTST_RAM, NULL,
			DTA_DataType, prevdt,
			DTA_GroupID, GID_PICTURE, // we only support images for now
			TAG_DONE)) != NULL)
	{
		ReleaseDataType(prevdt);
		prevdt = dt;
		ami_datatype_to_mimetype(dt, &dt_mime);

		LOG(("Guessed MIME from DT: %s", dt_mime));

		if(strcmp(dt_mime, mime_type) == 0)
		{
			found = true;
			break;
		}
	}

	ReleaseDataType(prevdt);

	return found;
}

#endif
