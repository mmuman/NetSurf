/*
 * Copyright 2011 Sven Weidauer <sven.weidauer@gmail.com>
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

#import <Cocoa/Cocoa.h>

#import "utils/nsoption.h"
#import "utils/utils.h"
#import "utils/log.h"
#import "utils/nsurl.h"
#import "netsurf/mouse.h"
#import "netsurf/window.h"
#import "netsurf/misc.h"
#import "netsurf/browser_window.h"
#import "netsurf/content.h"

#import "cocoa/gui.h"
#import "cocoa/coordinates.h"
#import "cocoa/plotter.h"
#import "cocoa/BrowserView.h"
#import "cocoa/BrowserViewController.h"
#import "cocoa/BrowserWindowController.h"
#import "cocoa/FormSelectMenu.h"
#import "cocoa/fetch.h"
#import "cocoa/schedule.h"


NSString * const kCookiesFileOption = @"CookiesFile";
NSString * const kURLsFileOption = @"URLsFile";
NSString * const kHotlistFileOption = @"Hotlist";
NSString * const kHomepageURLOption = @"HomepageURL";
NSString * const kOptionsFileOption = @"ClassicOptionsFile";
NSString * const kAlwaysCancelDownload = @"AlwaysCancelDownload";
NSString * const kAlwaysCloseMultipleTabs = @"AlwaysCloseMultipleTabs";

#define UNIMPL() NSLog( @"Function '%s' unimplemented", __func__ )

struct browser_window;

/* exported function docuemnted in cocoa/gui.h */
nserror cocoa_warning(const char *warning, const char *detail)
{
	NSRunAlertPanel( NSLocalizedString( @"Warning",
                                            @"Warning title" ),
                         NSLocalizedString( @"Warning %s%s%s",
                                            @"Warning message" ),
                         NSLocalizedString( @"OK", @"" ), nil, nil,
                         warning, detail != NULL ? ": " : "",
                         detail != NULL ? detail : "" );
        return NSERROR_OK;
}


static struct gui_window *
gui_window_create(struct browser_window *bw,
                  struct gui_window *existing,
                  gui_window_create_flags flags)
{
	BrowserWindowController *window = nil;
	BrowserViewController *result;

	browser_window_set_scale(bw, (float)nsoption_int(scale) / 100, false);
	if (existing != NULL) {
		window = [(BrowserViewController *)(existing) windowController];
	}

        result = [[BrowserViewController alloc] initWithBrowser: bw];

	if (!(flags & GW_CREATE_TAB) || nil == window) {
		window = [[[BrowserWindowController alloc] init] autorelease];
		[[window window] makeKeyAndOrderFront: nil];
	}
	[window addTab: result];
	
	return (struct gui_window *)result;
}

static void gui_window_destroy(struct gui_window *g)
{
	BrowserViewController *vc = (BrowserViewController *)g;
	[vc release];
}

static void gui_window_set_title(struct gui_window *g, const char *title)
{
	[(BrowserViewController *)g setTitle: [NSString stringWithUTF8String: title]];
}

static void gui_window_invalidate(struct gui_window *g, const struct rect *rect)
{
	if (rect == NULL) {
		[[(BrowserViewController *)g browserView] setNeedsDisplay: YES];
		return;
	}

	const NSRect nsrect = cocoa_scaled_rect_wh( 
		browser_window_get_scale([(BrowserViewController *)g browser]),
		rect->x0, rect->y0, 
		rect->x1 - rect->x0, rect->y1 - rect->y0 );
	[[(BrowserViewController *)g browserView] setNeedsDisplayInRect: nsrect];
}

static bool gui_window_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	NSCParameterAssert( g != NULL && sx != NULL && sy != NULL );
	
	NSRect visible = [[(BrowserViewController *)g browserView] visibleRect];
	*sx = cocoa_pt_to_px( NSMinX( visible ) );
	*sy = cocoa_pt_to_px( NSMinY( visible ) );
	return true;
}

static void gui_window_set_scroll(struct gui_window *g, int sx, int sy)
{
	[[(BrowserViewController *)g browserView] scrollPoint: cocoa_point( sx, sy )];
}



/**
 * Find the current dimensions of a cocoa browser window content area.
 *
 * \param gw The gui window to measure content area of.
 * \param width receives width of window
 * \param height receives height of window
 * \param scaled whether to return scaled values
 * \return NSERROR_OK on sucess and width and height updated
 *          else error code.
 */
static nserror gui_window_get_dimensions(struct gui_window *g,
                                      int *width, int *height,
                                      bool scaled)
{
	NSCParameterAssert( width != NULL && height != NULL );
	
	NSRect frame = [[[(BrowserViewController *)g browserView] superview] frame];
	if (scaled) {
        	const CGFloat scale = browser_window_get_scale([(BrowserViewController *)g browser]);
		frame.size.width /= scale;
		frame.size.height /= scale;
	}
	*width = cocoa_pt_to_px( NSWidth( frame ) );
	*height = cocoa_pt_to_px( NSHeight( frame ) );

        return NSERROR_OK;
}

static void gui_window_update_extent(struct gui_window *g)
{
	BrowserViewController * const window = (BrowserViewController *)g;
        int width;
        int height;
	struct browser_window *browser = [window browser];

        browser_window_get_extents(browser, false, &width, &height);
	
	[[window browserView] setMinimumSize:
                                cocoa_scaled_size( browser_window_get_scale(browser), width, height )];
}

static void gui_window_set_status(struct gui_window *g, const char *text)
{
	[(BrowserViewController *)g setStatus: [NSString stringWithUTF8String: text]];
}

static void gui_window_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	switch (shape) {
		case GUI_POINTER_DEFAULT:
		case GUI_POINTER_WAIT:
		case GUI_POINTER_PROGRESS:
			[[NSCursor arrowCursor] set];
			break;
			
		case GUI_POINTER_CROSS:
			[[NSCursor crosshairCursor] set];
			break;
			
		case GUI_POINTER_POINT:
		case GUI_POINTER_MENU:
			[[NSCursor pointingHandCursor] set];
			break;
			
		case GUI_POINTER_CARET:
			[[NSCursor IBeamCursor] set];
			break;
			
		case GUI_POINTER_MOVE:
			[[NSCursor closedHandCursor] set];
			break;

		default:
			NSLog( @"Other cursor %d requested", shape );
			[[NSCursor arrowCursor] set];
			break;
	}
}

static nserror gui_window_set_url(struct gui_window *g, struct nsurl *url)
{
        [(BrowserViewController *)g setUrl: [NSString stringWithUTF8String: nsurl_access(url)]];
        return NSERROR_OK;
}

static void gui_window_start_throbber(struct gui_window *g)
{
	[(BrowserViewController *)g setIsProcessing: YES];
	[(BrowserViewController *)g updateBackForward];
}

static void gui_window_stop_throbber(struct gui_window *g)
{
	[(BrowserViewController *)g setIsProcessing: NO];
	[(BrowserViewController *)g updateBackForward];
}

static void gui_window_set_icon(struct gui_window *g, struct hlcache_handle *icon)
{
	NSBitmapImageRep *bmp = NULL;
	NSImage *image = nil;

	if (icon != NULL) {
		bmp = (NSBitmapImageRep *)content_get_bitmap( icon );
	}

	if (bmp != nil) {
		image = [[NSImage alloc] initWithSize: NSMakeSize( 32, 32 )];
		[image addRepresentation: bmp];
	} else {
		image = [[NSImage imageNamed: @"NetSurf"] copy];
	}
	[image setFlipped: YES];

	[(BrowserViewController *)g setFavicon: image];
	[image release];
}

static void
gui_window_place_caret(struct gui_window *g, int x, int y, int height,
                       const struct rect *clip)
{
	[[(BrowserViewController *)g browserView]
                addCaretAt: cocoa_point( x, y )
                    height: cocoa_px_to_pt( height )];
}

static void gui_window_remove_caret(struct gui_window *g)
{
	[[(BrowserViewController *)g browserView] removeCaret];
}

static void gui_window_new_content(struct gui_window *g)
{
	[(BrowserViewController *)g contentUpdated];
}

/**
 * process miscellaneous window events
 *
 * \param gw The window receiving the event.
 * \param event The event code.
 * \return NSERROR_OK when processed ok
 */
static nserror
gui_window_event(struct gui_window *gw, enum gui_window_event event)
{
	switch (event) {
	case GW_EVENT_UPDATE_EXTENT:
		gui_window_update_extent(gw);
		break;

	case GW_EVENT_REMOVE_CARET:
		gui_window_remove_caret(gw);
		break;

	case GW_EVENT_NEW_CONTENT:
		gui_window_new_content(gw);
		break;

	case GW_EVENT_START_THROBBER:
		gui_window_start_throbber(gw);
		break;

	case GW_EVENT_STOP_THROBBER:
		gui_window_stop_throbber(gw);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}


static void gui_create_form_select_menu(struct gui_window *g,
                                        struct form_control *control)
{
	BrowserViewController * const window = (BrowserViewController *)g;
	FormSelectMenu *menu = [[FormSelectMenu alloc]
                                        initWithControl: control
                                              forWindow: [window browser]];
	[menu runInView: [window browserView]];
	[menu release];
}

static nserror gui_launch_url(nsurl *url)
{
	[[NSWorkspace sharedWorkspace] openURL: [NSURL URLWithString: [NSString stringWithUTF8String: nsurl_access(url)]]];
    return NSERROR_OK;
}

struct ssl_cert_info;

static nserror
gui_cert_verify(nsurl *url,
                const struct ssl_cert_info *certs,
                unsigned long num,
                nserror (*cb)(bool proceed,void *pw), void *cbpw)
{
	return NSERROR_NOT_IMPLEMENTED;
}


static struct gui_window_table window_table = {
	.create = gui_window_create,
	.destroy = gui_window_destroy,
	.invalidate = gui_window_invalidate,
	.get_scroll = gui_window_get_scroll,
	.set_scroll = gui_window_set_scroll,
	.get_dimensions = gui_window_get_dimensions,
	.event = gui_window_event,

	.set_title = gui_window_set_title,
	.set_url = gui_window_set_url,
	.set_icon = gui_window_set_icon,
	.set_status = gui_window_set_status,
	.set_pointer = gui_window_set_pointer,
	.place_caret = gui_window_place_caret,
	.create_form_select_menu = gui_create_form_select_menu,
};

struct gui_window_table *cocoa_window_table = &window_table;


static struct gui_misc_table browser_table = {
	.schedule = cocoa_schedule,

	.launch_url = gui_launch_url,
	.cert_verify = gui_cert_verify,
};

struct gui_misc_table *cocoa_misc_table = &browser_table;
