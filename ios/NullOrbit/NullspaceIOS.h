#pragma once

// C interface between the Obj-C view layer and the C++ game bridge.
// Keeps nullspace_ios.mm self-contained — ViewController only needs this header.

#ifdef __cplusplus
extern "C" {
#endif

// Call once after the EAGLLayer is configured. layer is a CAEAGLLayer*.
void iOSInit(void* eagl_layer, int physical_width, int physical_height, float screen_scale);

// Render one frame and present. Called by CADisplayLink.
void iOSTick(void);

// Full teardown (app termination).
void iOSShutdown(void);

// Returns 1 if the game is running (i.e. past the main menu).
int iOSIsInGame(void);

// Called from UIKit menu buttons. server_index matches kServers[] in nullspace_ios.mm.
// arena_name: "pub", "tdm", or "" for auto.
void iOSJoinZone(int server_index, const char* arena_name);

// Touch events — physical pixel coordinates, screen_width/height are physical pixels.
void iOSTouchBegan(float x, float y, long pointer_id, int screen_width, int screen_height);
void iOSTouchMoved(float x, float y, long pointer_id, int screen_width, int screen_height);
void iOSTouchEnded(float x, float y, long pointer_id, int screen_width, int screen_height);
void iOSTouchCancelled(int screen_width, int screen_height);

// Username used on the main menu (written by game bridge, read by ViewController).
const char* iOSGetUsername(void);
void iOSRegenerateUsername(void);

#ifdef __cplusplus
}
#endif
