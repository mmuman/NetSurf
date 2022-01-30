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
#import "utils/messages.h"
#import "utils/nsurl.h"
#import "utils/utils.h"
#import "netsurf/browser_window.h"
#import "netsurf/plotters.h"
#import "netsurf/content.h"
#import "netsurf/keypress.h"

#import "cocoa/gui.h"
#import "cocoa/BrowserView.h"
#import "cocoa/HistoryView.h"
#import "cocoa/font.h"
#import "cocoa/coordinates.h"
#import "cocoa/plotter.h"
#import "cocoa/LocalHistoryController.h"
#import "cocoa/BrowserWindowController.h"


@interface BrowserView ()

@property (readwrite, copy, nonatomic) NSString *markedText;

- (void) scrollHorizontal: (CGFloat) amount;
- (void) scrollVertical: (CGFloat) amount;
- (CGFloat) pageScroll;

- (void) popUpContextMenuForEvent: (NSEvent *) event;

- (IBAction) cmOpenURLInTab: (id) sender;
- (IBAction) cmOpenURLInWindow: (id) sender;
- (IBAction) cmDownloadURL: (id) sender;

- (IBAction) cmLinkCopy: (id) sender;
- (IBAction) cmImageCopy: (id) sender;

@end

@implementation BrowserView

@synthesize browser;
@synthesize caretTimer;
@synthesize markedText;

static const CGFloat CaretWidth = 1.0;
static const NSTimeInterval CaretBlinkTime = 0.8;

- initWithFrame: (NSRect) frame
{
        if ((self = [super initWithFrame: frame]) == nil) {
                return nil;
        }

        [self registerForDraggedTypes: [NSArray arrayWithObjects: NSURLPboardType, @"public.url", nil]];

        return self;
}

- (void) dealloc
{
        [self setCaretTimer: nil];
        [self setMarkedText: nil];
        [history release];

        [super dealloc];
}

- (void) setCaretTimer: (NSTimer *)newTimer
{
        if (newTimer != caretTimer) {
                [caretTimer invalidate];
                [caretTimer release];
                caretTimer = [newTimer retain];
        }
}

- (void) updateHistory
{
        [history redraw];
}

static inline NSRect cocoa_get_caret_rect( BrowserView *view )
{
        float bscale = browser_window_get_scale(view->browser);

        NSRect caretRect = {
                .origin = NSMakePoint( view->caretPoint.x * bscale, view->caretPoint.y * bscale ),
                .size = NSMakeSize( CaretWidth, view->caretHeight * bscale )
        };

        return caretRect;
}

- (void) removeCaret
{
        hasCaret = NO;
        [self setNeedsDisplayInRect: cocoa_get_caret_rect( self )];

        [self setCaretTimer: nil];
}

- (void) addCaretAt: (NSPoint) point height: (CGFloat) height
{
        if (hasCaret) {
                [self setNeedsDisplayInRect: cocoa_get_caret_rect( self )];
        }

        caretPoint = point;
        caretHeight = height;
        hasCaret = YES;
        caretVisible = YES;

        if (nil == caretTimer) {
                [self setCaretTimer: [NSTimer scheduledTimerWithTimeInterval: CaretBlinkTime target: self selector: @selector(caretBlink:) userInfo: nil repeats: YES]];
        } else {
                [caretTimer setFireDate: [NSDate dateWithTimeIntervalSinceNow: CaretBlinkTime]];
        }

        [self setNeedsDisplayInRect: cocoa_get_caret_rect( self )];
}


- (void) caretBlink: (NSTimer *)timer
{
        if (hasCaret) {
                caretVisible = !caretVisible;
                [self setNeedsDisplayInRect: cocoa_get_caret_rect( self )];
        }
}

- (void)drawRect:(NSRect)dirtyRect
{
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

        struct redraw_context ctx = {
                .interactive = true,
                .background_images = true,
                .plot = &cocoa_plotters
        };

        const NSRect *rects = NULL;
        NSInteger count = 0;
        [self getRectsBeingDrawn: &rects count: &count];

        for (NSInteger i = 0; i < count; i++) {
                const struct rect clip = {
                        .x0 = cocoa_pt_to_px( NSMinX( rects[i] ) ),
                        .y0 = cocoa_pt_to_px( NSMinY( rects[i] ) ),
                        .x1 = cocoa_pt_to_px( NSMaxX( rects[i] ) ),
                        .y1 = cocoa_pt_to_px( NSMaxY( rects[i] ) )
                };

                browser_window_redraw(browser, 0, 0, &clip, &ctx);
        }

        NSRect caretRect = cocoa_get_caret_rect( self );
        if (hasCaret && caretVisible && [self needsToDrawRect: caretRect]) {
                [[NSColor blackColor] set];
                [NSBezierPath fillRect: caretRect];
        }

        [pool release];
}

- (BOOL) isFlipped
{
        return YES;
}

- (void) viewDidMoveToWindow
{
#ifndef GNUSTEP
        NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect: [self visibleRect]
                                                            options: NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                              owner: self
                                                           userInfo: nil];
        [self addTrackingArea: area];
        [area release];
#endif
}

static browser_mouse_state cocoa_mouse_flags_for_event( NSEvent *evt )
{
        browser_mouse_state result = 0;
        NSUInteger flags = [evt modifierFlags];

        if (flags & NSShiftKeyMask) result |= BROWSER_MOUSE_MOD_1;
        if (flags & NSAlternateKeyMask) result |= BROWSER_MOUSE_MOD_2;

        return result;
}

- (NSPoint) convertMousePoint: (NSEvent *)event
{
        NSPoint location = [self convertPoint: [event locationInWindow] fromView: nil];
        float bscale = browser_window_get_scale(browser);

        location.x /= bscale;
        location.y /= bscale;

        location.x = cocoa_pt_to_px( location.x );
        location.y = cocoa_pt_to_px( location.y );
        return location;
}

- (void) mouseDown: (NSEvent *)theEvent
{
        if ([theEvent modifierFlags] & NSControlKeyMask) {
                [self popUpContextMenuForEvent: theEvent];
                return;
        }

        dragStart = [self convertMousePoint: theEvent];

        browser_window_mouse_click(browser,
                                   BROWSER_MOUSE_PRESS_1 | cocoa_mouse_flags_for_event( theEvent ),
                                   dragStart.x,
                                   dragStart.y );
}

- (void) rightMouseDown: (NSEvent *)theEvent
{
        [self popUpContextMenuForEvent: theEvent];
}

- (void) mouseUp: (NSEvent *)theEvent
{
        if (historyVisible) {
                [self setHistoryVisible: NO];
                return;
        }

        NSPoint location = [self convertMousePoint: theEvent];

        browser_mouse_state modifierFlags = cocoa_mouse_flags_for_event( theEvent );

        if (isDragging) {
                isDragging = NO;
                browser_window_mouse_track( browser, (browser_mouse_state)0, location.x, location.y );
        } else {
                modifierFlags |= BROWSER_MOUSE_CLICK_1;
                if ([theEvent clickCount] == 2) modifierFlags |= BROWSER_MOUSE_DOUBLE_CLICK;
                browser_window_mouse_click( browser, modifierFlags, location.x, location.y );
        }
}

#define squared(x) ((x)*(x))
#define MinDragDistance (5.0)

- (void) mouseDragged: (NSEvent *)theEvent
{
        NSPoint location = [self convertMousePoint: theEvent];
        browser_mouse_state modifierFlags = cocoa_mouse_flags_for_event( theEvent );

        if (!isDragging) {
                const CGFloat distance = squared( dragStart.x - location.x ) + squared( dragStart.y - location.y );

                if (distance >= squared( MinDragDistance)) {
                        isDragging = YES;
                        browser_window_mouse_click(browser,
                                                   BROWSER_MOUSE_DRAG_1 | modifierFlags,
                                                   dragStart.x,
                                                   dragStart.y);
                }
        }

        if (isDragging) {
                browser_window_mouse_track(browser,
                                           BROWSER_MOUSE_HOLDING_1 | BROWSER_MOUSE_DRAG_ON | modifierFlags,
                                           location.x,
                                           location.y );
        }
}

- (void) mouseMoved: (NSEvent *)theEvent
{
        if (historyVisible) return;

        NSPoint location = [self convertMousePoint: theEvent];

        browser_window_mouse_track(browser,
                                   cocoa_mouse_flags_for_event(theEvent),
                                   location.x,
                                   location.y);
}

- (void) mouseExited: (NSEvent *) theEvent
{
        [[NSCursor arrowCursor] set];
}

- (void) keyDown: (NSEvent *)theEvent
{
        if (!historyVisible) {
                [self interpretKeyEvents: [NSArray arrayWithObject: theEvent]];
        } else {
                [history keyDown: theEvent];
        }
}

- (void) insertText: (id)string
{
        for (NSUInteger i = 0, length = [string length]; i < length; i++) {
                unichar ch = [string characterAtIndex: i];
                if (!browser_window_key_press( browser, ch )) {
                        if (ch == ' ') [self scrollPageDown: self];
                        break;
                }
        }
        [self setMarkedText: nil];
}

- (void) moveLeft: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_LEFT )) return;
        [self scrollHorizontal: -[[self enclosingScrollView] horizontalLineScroll]];
}

- (void) moveRight: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_RIGHT )) return;
        [self scrollHorizontal: [[self enclosingScrollView] horizontalLineScroll]];
}

- (void) moveUp: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_UP )) return;
        [self scrollVertical: -[[self enclosingScrollView] lineScroll]];
}

- (void) moveDown: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_DOWN )) return;
        [self scrollVertical: [[self enclosingScrollView] lineScroll]];
}

- (void) deleteBackward: (id)sender
{
        if (!browser_window_key_press( browser, NS_KEY_DELETE_LEFT )) {
                [NSApp sendAction: @selector( goBack: ) to: nil from: self];
        }
}

- (void) deleteForward: (id)sender
{
        browser_window_key_press( browser, NS_KEY_DELETE_RIGHT );
}

- (void) cancelOperation: (id)sender
{
        browser_window_key_press( browser, NS_KEY_ESCAPE );
}

- (void) scrollPageUp: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_PAGE_UP )) {
                return;
        }
        [self scrollVertical: -[self pageScroll]];
}

- (void) scrollPageDown: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_PAGE_DOWN )) {
                return;
        }
        [self scrollVertical: [self pageScroll]];
}

- (void) insertTab: (id)sender
{
        browser_window_key_press( browser, NS_KEY_TAB );
}

- (void) insertBacktab: (id)sender
{
        browser_window_key_press( browser, NS_KEY_SHIFT_TAB );
}

- (void) moveToBeginningOfLine: (id)sender
{
        browser_window_key_press( browser, NS_KEY_LINE_START );
}

- (void) moveToEndOfLine: (id)sender
{
        browser_window_key_press( browser, NS_KEY_LINE_END );
}

- (void) moveToBeginningOfDocument: (id)sender
{
        if (browser_window_key_press( browser, NS_KEY_TEXT_START )) return;
}

- (void) scrollToBeginningOfDocument: (id) sender
{
        NSPoint origin = [self visibleRect].origin;
        origin.y = 0;
        [self scrollPoint: origin];
}

- (void) moveToEndOfDocument: (id)sender
{
        browser_window_key_press( browser, NS_KEY_TEXT_END );
}

- (void) scrollToEndOfDocument: (id) sender
{
        NSPoint origin = [self visibleRect].origin;
        origin.y = NSHeight( [self frame] );
        [self scrollPoint: origin];
}

- (void) insertNewline: (id)sender
{
        browser_window_key_press( browser, NS_KEY_NL );
}

- (void) selectAll: (id)sender
{
        browser_window_key_press( browser, NS_KEY_SELECT_ALL );
}

- (void) copy: (id)sender
{
        browser_window_key_press( browser, NS_KEY_COPY_SELECTION );
}

- (void) cut: (id)sender
{
        browser_window_key_press( browser, NS_KEY_CUT_SELECTION );
}

- (void) paste: (id)sender
{
        browser_window_key_press( browser, NS_KEY_PASTE );
}

- (BOOL) acceptsFirstResponder
{
        return YES;
}

- (void) adjustFrame
{
        browser_window_schedule_reformat(browser);

        [super adjustFrame];
}

- (BOOL) isHistoryVisible
{
        return historyVisible;
}

- (void) setHistoryVisible: (BOOL)newVisible
{
        if (newVisible == historyVisible) return;
        historyVisible = newVisible;

        if (historyVisible) {
                if (nil == history) {
                        history = [[LocalHistoryController alloc] initWithBrowser: self];
                }
                [history attachToView: [(BrowserWindowController *)[[self window] windowController] historyButton]];
        } else {
                [history detach];
        }
}

- (void) scrollHorizontal: (CGFloat) amount
{
        NSPoint currentPoint = [self visibleRect].origin;
        currentPoint.x += amount;
        [self scrollPoint: currentPoint];
}

- (void) scrollVertical: (CGFloat) amount
{
        NSPoint currentPoint = [self visibleRect].origin;
        currentPoint.y += amount;
        [self scrollPoint: currentPoint];
}

- (CGFloat) pageScroll
{
        return NSHeight( [[self superview] frame] ) - [[self enclosingScrollView] pageScroll];
}


- (void) popUpContextMenuForEvent: (NSEvent *) event
{
        NSMenu *popupMenu = [[NSMenu alloc] initWithTitle: @""];
        NSPoint point = [self convertMousePoint: event];

        struct browser_window_features cont;

        browser_window_get_features(browser, point.x, point.y, &cont);

        if (cont.object != NULL) {
                NSString *imageURL = [NSString stringWithUTF8String: nsurl_access(hlcache_handle_get_url( cont.object ))];

                [[popupMenu addItemWithTitle: NSLocalizedString( @"Open image in new tab", @"Context menu" )
                                      action: @selector(cmOpenURLInTab:)
                               keyEquivalent: @""] setRepresentedObject: imageURL];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Open image in new window", @"Context menu" )
                                      action: @selector(cmOpenURLInWindow:)
                               keyEquivalent: @""] setRepresentedObject: imageURL];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Save image as", @"Context menu" )
                                      action: @selector(cmDownloadURL:)
                               keyEquivalent: @""] setRepresentedObject: imageURL];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Copy image", @"Context menu" )
                                      action: @selector(cmImageCopy:)
                               keyEquivalent: @""] setRepresentedObject: (id)content_get_bitmap( cont.object )];

                [popupMenu addItem: [NSMenuItem separatorItem]];
        }

        if (cont.link != NULL) {
                NSString *target = [NSString stringWithUTF8String: nsurl_access(cont.link)];

                [[popupMenu addItemWithTitle: NSLocalizedString( @"Open link in new tab", @"Context menu" )
                                      action: @selector(cmOpenURLInTab:)
                               keyEquivalent: @""] setRepresentedObject: target];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Open link in new window", @"Context menu" )
                                      action: @selector(cmOpenURLInWindow:)
                               keyEquivalent: @""] setRepresentedObject: target];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Save link target", @"Context menu" )
                                      action: @selector(cmDownloadURL:)
                               keyEquivalent: @""] setRepresentedObject: target];
                [[popupMenu addItemWithTitle: NSLocalizedString( @"Copy link", @"Context menu" )
                                      action: @selector(cmLinkCopy:)
                               keyEquivalent: @""] setRepresentedObject: target];

                [popupMenu addItem: [NSMenuItem separatorItem]];
        }

        [popupMenu addItemWithTitle: NSLocalizedString( @"Back", @"Context menu" )
                             action: @selector(goBack:) keyEquivalent: @""];
        [popupMenu addItemWithTitle: NSLocalizedString( @"Reload", @"Context menu" )
                             action: @selector(reloadPage:) keyEquivalent: @""];
        [popupMenu addItemWithTitle: NSLocalizedString( @"Forward", @"Context menu" )
                             action: @selector(goForward:) keyEquivalent: @""];
        [popupMenu addItemWithTitle: NSLocalizedString( @"View Source", @"Context menu" )
                             action: @selector(viewSource:) keyEquivalent: @""];

        [NSMenu popUpContextMenu: popupMenu withEvent: event forView: self];

        [popupMenu release];
}

- (IBAction) cmOpenURLInTab: (id)sender
{
        nsurl *url;
        nserror error;

        error = nsurl_create([[sender representedObject] UTF8String], &url);
        if (error == NSERROR_OK) {
                error = browser_window_create(BW_CREATE_HISTORY |
                                              BW_CREATE_TAB |
                                              BW_CREATE_CLONE,
                                              url,
                                              NULL,
                                              browser,
                                              NULL);
                nsurl_unref(url);
        }
        if (error != NSERROR_OK) {
                cocoa_warning(messages_get_errorcode(error), 0);
        }
}

- (IBAction) cmOpenURLInWindow: (id)sender
{
        nsurl *url;
        nserror error;

        error = nsurl_create([[sender representedObject] UTF8String], &url);
        if (error == NSERROR_OK) {
                error = browser_window_create(BW_CREATE_HISTORY |
                                              BW_CREATE_CLONE,
                                              url,
                                              NULL,
                                              browser,
                                              NULL);
                nsurl_unref(url);
        }
        if (error != NSERROR_OK) {
                cocoa_warning(messages_get_errorcode(error), 0);
        }
}

- (IBAction) cmDownloadURL: (id)sender
{
        nsurl *url;

        if (nsurl_create([[sender representedObject] UTF8String], &url) == NSERROR_OK) {
                browser_window_navigate(browser,
                                        url,
                                        NULL,
                                        BW_NAVIGATE_DOWNLOAD,
                                        NULL,
                                        NULL,
                                        NULL);
                nsurl_unref(url);
        }
}

- (IBAction) cmImageCopy: (id)sender
{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb declareTypes: [NSArray arrayWithObject: NSTIFFPboardType] owner: nil];
        [pb setData: [[sender representedObject] TIFFRepresentation] forType: NSTIFFPboardType];
}

- (IBAction) cmLinkCopy: (id)sender
{
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb declareTypes: [NSArray arrayWithObject: NSStringPboardType] owner: nil];
        [pb setString: [sender representedObject] forType: NSStringPboardType];
}


// MARK: -
// MARK: Accepting dragged URLs

- (NSDragOperation)draggingEntered:(id <NSDraggingInfo>)sender
{
        if ((NSDragOperationCopy | NSDragOperationGeneric) & [sender draggingSourceOperationMask]) {
                return NSDragOperationCopy;
        }

        return NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id <NSDraggingInfo>)sender
{
        return YES;
}

- (BOOL)performDragOperation:(id <NSDraggingInfo>)sender
{
        nsurl *url;
        nserror error;

        NSPasteboard *pb = [sender draggingPasteboard];

        NSString *type = [pb availableTypeFromArray:[NSArray arrayWithObjects: @"public.url", NSURLPboardType,  nil]];

        NSString *urlstr = nil;

        if ([type isEqualToString: NSURLPboardType]) {
                urlstr = [[NSURL URLFromPasteboard: pb] absoluteString];
        } else {
                urlstr = [pb stringForType: type];
        }

        error = nsurl_create([urlstr UTF8String], &url);
        if (error != NSERROR_OK) {
                cocoa_warning(messages_get_errorcode(error), 0);
        } else {
                browser_window_navigate(browser,
                                        url,
                                        NULL,
                                        BW_NAVIGATE_DOWNLOAD,
                                        NULL,
                                        NULL,
                                        NULL);
                nsurl_unref(url);
        }

        return YES;
}

// MARK: -
// MARK: NSTextInput protocol implementation

- (void) setMarkedText: (id) aString selectedRange: (NSRange) selRange
{
        [markedText release];
        markedText = [aString isEqualToString: @""] ? nil : [aString copy];
}

- (void) unmarkText
{
        [self setMarkedText: nil];
}

- (BOOL) hasMarkedText
{
        return markedText != nil;
}

- (NSInteger) conversationIdentifier
{
        return (NSInteger)self;
}

- (NSAttributedString *) attributedSubstringFromRange: (NSRange) theRange
{
        return [[[NSAttributedString alloc] initWithString: @""] autorelease];
}

- (NSRange) markedRange
{
        return NSMakeRange( NSNotFound, 0 );
}

- (NSRange) selectedRange
{
        return NSMakeRange( NSNotFound, 0 );
}

- (NSRect) firstRectForCharacterRange: (NSRange) theRange
{
        return NSZeroRect;
}

- (NSUInteger) characterIndexForPoint: (NSPoint) thePoint
{
        return 0;
}

- (NSArray *) validAttributesForMarkedText
{
        return [NSArray array];
}

- (void) doCommandBySelector: (SEL) sel;
{
        [super doCommandBySelector: sel];
}

@end
