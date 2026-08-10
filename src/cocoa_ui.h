#ifndef PPC_SPOTI_COCOA_UI_H
#define PPC_SPOTI_COCOA_UI_H

/*
 * The real Cocoa UI: device-name field + Save, and a pure now-playing
 * display (album art/title/artist/album/connection info) - no local media
 * control, playback is driven entirely by the connected AirPlay sender.
 * This is a plain C entry point so ppc_pod_gui_main.m doesn't need to know
 * any of cocoa_ui.m's Objective-C internals.
 *
 * Blocks for the lifetime of the app (calls [NSApp run] internally) - the
 * AirPlay backend thread (receiver_entry.h) must already be running before
 * this is called, since this function does not return until the app
 * quits.
 */
void cocoa_ui_run(void);

#endif
