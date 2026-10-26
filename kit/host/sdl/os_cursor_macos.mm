// Run 27 (owner: the mouse cursor did not auto-hide; review/run27/l3-cursor-findings.md): SDL 3 on macOS hides the
// cursor only through the view's cursor rects, which AppKit applies when the pointer moves inside the key window, so
// an idle pointer kept the arrow. The host's reconcile_os_cursor (main.cpp) calls this beside SDL_HideCursor /
// SDL_ShowCursor, once per transition (NSCursor's hide count stays balanced). Main thread only, as the host's cursor
// state already is.
#import <AppKit/AppKit.h>

void os_cursor_set_hidden(bool hidden) {
    static bool is_hidden = false;
    if (hidden == is_hidden)
        return;
    if (hidden)
        [NSCursor hide];
    else
        [NSCursor unhide];
    is_hidden = hidden;
}
