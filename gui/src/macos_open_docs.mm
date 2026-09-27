/*
 * macos_open_docs - files opened through Finder, on macOS.
 *
 * There are two ways to hand this app a file, and they arrive by different
 * routes:
 *
 *   dragged onto the WINDOW    GLFW's drop callback. Works everywhere.
 *   double-clicked in Finder,  an Apple Event, kAEOpenDocuments. A .app
 *   dropped on the DOCK icon,  launched from Finder is never handed its
 *   or "Open With"             documents in argv.
 *
 * The three Finder gestures are all the same event, so they stand or fall
 * together. Half the job is here; the other half is the CFBundleDocumentTypes
 * claim in Info.plist.in, because macOS sends these events only for types an
 * app has claimed.
 *
 * WHY A DELEGATE METHOD AND NOT AN APPLE EVENT HANDLER
 *
 * Registering with NSAppleEventManager directly looks like the obvious
 * approach and does not work here. glfwInit() calls [NSApp run] itself (see
 * cocoa_init.m), so by the time it returns, AppKit has finished launching:
 * it has installed its OWN kAEOpenDocuments handler and has already processed
 * the document the app was launched to open. A handler registered afterwards
 * replaces AppKit's for later events but arrives too late for that first one,
 * and AppKit -- finding no -application:openFiles: on the delegate -- has by
 * then put up "cannot open files in the ... format".
 *
 * So this goes with the grain instead: AppKit's handler is left in place and
 * given the delegate method it looks for. That covers the launch document and
 * every later drop through one path.
 *
 * GLFW owns the delegate and does not implement the method, so it is added to
 * GLFW's delegate CLASS at runtime. Done before glfwInit(), while the class
 * exists but no instance does, so it is in place before AppKit ever looks.
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "macos_open_docs.h"

static apollo_open_fn g_open = 0;

/* -application:openFiles: , spliced onto whichever class owns the delegate. */
static void open_files_imp(id self, SEL cmd, NSApplication *app, NSArray *files)
{
    (void)self;
    (void)cmd;

    for (NSString *path in files) {
        if (g_open && [path length])
            g_open([path UTF8String]);
    }

    /* AppKit requires an answer from this method; without one it can decide
     * the open failed and say so. */
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

static int install(Class cls)
{
    if (!cls)
        return 0;
    /* NO when the class already has it, which is success as far as this is
     * concerned -- it means a second call found the work already done. */
    class_addMethod(cls, @selector(application:openFiles:),
                    (IMP)open_files_imp, "v@:@@");
    return class_getInstanceMethod(cls, @selector(application:openFiles:)) != NULL;
}

int apollo_macos_watch_open_documents(apollo_open_fn cb)
{
    g_open = cb;

    /* By name, before glfwInit(): the class is compiled into this binary, so
     * it exists well before GLFW makes an instance of it. This is the call
     * that catches a double-click launch. */
    int ok = install(objc_getClass("GLFWApplicationDelegate"));

    /* And by instance, for a second call after glfwInit() -- which covers
     * GLFW ever renaming that class out from under the lookup above. */
    if (NSApp && [NSApp delegate])
        ok |= install(object_getClass([NSApp delegate]));

    return ok;
}
