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

#import "utils/nsoption.h"
#import "utils/nsurl.h"
#import "netsurf/browser_window.h"

#import "cocoa/PreferencesWindowController.h"
#import "cocoa/NetsurfApp.h"
#import "cocoa/gui.h"
#import "cocoa/BrowserViewController.h"

@implementation PreferencesWindowController

- init;
{
	if ((self = [super initWithWindowNibName: @"PreferencesWindow"]) == nil) return nil;
	
	return self;
}

- (IBAction) useCurrentPageAsHomepage: (id) sender;
{
	struct browser_window *bw = [[(NetSurfApp *)NSApp frontTab] browser];
	const char *url = nsurl_access(browser_window_get_url(bw));
	[self setHomepageURL: [NSString stringWithUTF8String: url]];
}

- (void) setHomepageURL: (NSString *) newUrl;
{
	nsoption_set_charp(homepage_url, strdup( [newUrl UTF8String] ));
	[[NSUserDefaults standardUserDefaults] setObject: newUrl forKey: kHomepageURLOption];
	[[NSUserDefaults standardUserDefaults] synchronize];
}

- (NSString *) homepageURL;
{
	return [NSString stringWithUTF8String: nsoption_charp(homepage_url)];
}

@end
