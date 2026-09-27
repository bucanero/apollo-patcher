/*
 * See macos_open_docs.mm. macOS only; nothing else calls it.
 */
#ifndef APOLLO_MACOS_OPEN_DOCS_H
#define APOLLO_MACOS_OPEN_DOCS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the main thread, once per file, while events are being pumped --
 * the same thread and the same moment as GLFW's drop callback, so a handler
 * may touch app state directly. */
typedef void (*apollo_open_fn)(const char *path);

/*
 * Start delivering the files Finder asks the app to open. Nonzero when the
 * hook is in place.
 *
 * Call it TWICE: once before glfwInit(), which is what catches the document a
 * double-click launched the app with, and once after, which covers GLFW
 * renaming the delegate class the first call looks up by name. Idempotent.
 */
int apollo_macos_watch_open_documents(apollo_open_fn cb);

#ifdef __cplusplus
}
#endif

#endif /* APOLLO_MACOS_OPEN_DOCS_H */
