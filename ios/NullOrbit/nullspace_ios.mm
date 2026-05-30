// nullspace_ios.mm
// iOS game bridge — mirrors nullspace_android.cpp.
// Uses EAGL + CAEAGLLayer instead of EGL/ANativeWindow.
// Main menu is handled by UIKit (ViewController.mm), not ImGui.
//
// Include order:  ObjC/Foundation FIRST, then glad (which needs to be after
// the ObjC runtime is set up), then C++ game headers.

#define GLES_SILENCE_DEPRECATION

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#import <QuartzCore/CAEAGLLayer.h>
#import <dlfcn.h>

#ifndef NDEBUG
#define NULLSPACE_IOS_DEBUG_LOG 1
#else
#define NULLSPACE_IOS_DEBUG_LOG 0
#endif

// glad after ObjC headers; it will define __gl_h_ etc. to block later re-inclusion.
#include <glad/glad.h>

#include "NullspaceIOS.h"

#include <null/Game.h>
#include <null/Memory.h>
#include <null/Platform.h>
#include <null/net/Connection.h>
#include <null/ios/IOSSettings.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>

using namespace null;

static void* ios_gl_get_proc(const char* name) {
  static void* lib = nullptr;
  if (!lib)
    lib = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY);
  return dlsym(lib, name);
}

// ── EAGLContext / framebuffer globals ────────────────────────────────────────
static EAGLContext* g_Context          = nil;
static GLuint       g_Framebuffer      = 0;
static GLuint       g_ColorRB          = 0;
static GLuint       g_DepthRB          = 0;
static int          g_PhysicalWidth    = 0;
static int          g_PhysicalHeight   = 0;
static int          g_ViewportX        = 0;
static int          g_ViewportY        = 0;
static int          g_ViewportWidth    = 0;
static int          g_ViewportHeight   = 0;
static bool         g_Initialized      = false;

// ── touch input state ────────────────────────────────────────────────────────
static const int64_t DOUBLE_TAP_TIMEOUT  = 300LL * 1000000LL;  // ns
static const int64_t LONG_PRESS_TIMEOUT  = 800LL * 1000000LL;  // ns
static const int32_t DOUBLE_TAP_SLOP     = 100;

static struct {
  float world_x, world_y;
  float nx, ny;
  bool  anchored;

  int64_t last_tap_time;
  float   last_tap_x, last_tap_y;

  int64_t touch_start_time;
  float   touch_start_x, touch_start_y;
  bool    long_press_triggered;

  bool abilities_triggered;
  bool mine_mode_active;

  float joystick_x, joystick_y;
  bool  joystick_active;

  // Multi-touch slot tracking: left half = joystick, right half = fire/abilities
  long  left_ptr  = -1;   // pointer driving movement (left screen half)
  long  right_ptr = -1;   // pointer driving fire/abilities (right screen half)
} ios_input;

static inline int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ── forward declarations ─────────────────────────────────────────────────────
static void SetPlatform();

// ── server list (mirrors Android) ────────────────────────────────────────────
namespace null {

bool g_MobileMineMode = false;

GameSettings g_Settings;

void InitializeSettings() {
  g_Settings.vsync                = true;
  g_Settings.window_type          = WindowType::Windowed;
  g_Settings.render_stars         = true;
  g_Settings.encrypt_method       = EncryptMethod::Subspace;
  g_Settings.sound_enabled        = true;
  g_Settings.sound_volume         = 0.25f;
  g_Settings.sound_radius_increase= 10.0f;
  g_Settings.notify_max_prizes    = false;
  g_Settings.target_bounty        = 20;
}

}  // namespace null

namespace null {

const char* kPlayerPassword = "none";
const char* kArenaName      = "";

struct ServerInfo {
  const char* name;
  const char* server;
  u16 port;
};

static ServerInfo kServers[] = {
    {"simulator", "127.0.0.1", 5000},
    {"Null Orbit", "api.null-orbit.com", 5000},
};

const char* kPlayerName = "null space";
static const char* kServerIp   = kServers[1].server;
static u16         kServerPort = kServers[1].port;
const char* kServerName = kServers[1].name;

MemoryArena* perm_global = nullptr;

InputState g_InputState;

}  // namespace null

enum class GameScreen { MainMenu, Playing };

// ── nullspace game struct ─────────────────────────────────────────────────────
struct nullspace_ios_state {
  null::MemoryArena perm_arena;
  null::MemoryArena trans_arena;
  null::MemoryArena work_arena;
  null::WorkQueue*  work_queue  = nullptr;
  null::Worker*     worker      = nullptr;
  null::Game*       game        = nullptr;
  int  surface_width  = 0;
  int  surface_height = 0;
  int  physical_width = 0;
  int  physical_height= 0;
  char name[20]     = {};
  char password[20] = {};
  GameScreen screen = GameScreen::MainMenu;
  float frame_time  = 0.0f;
  float scale       = 1.0f;

  std::string GenerateRandomUsername() {
    static const char* first[] = {
        "Void","Nova","Storm","Shadow","Frost","Flame","Thunder","Plasma",
        "Eclipse","Meteor","Nebula","Ghost","Viper","Titan","Phoenix",
        "Raven","Wolf","Hawk","Comet","Pulsar","Photon","Zenith",
        "Apex","Vector","Neutron","Crimson","Azure","Iron","Dark","Solar"};
    static const char* second[] = {
        "Strike","Surge","Blast","Rush","Pierce","Crash","Dive","Soar",
        "Bolt","Slash","Burn","Shock","Dash","Rip","Tear","Hunt",
        "Forge","Break","Burst","Flash","Drift","Glide","Freeze","Shift",
        "Spark","Pulse","Fire","Charge","Smash","Raze"};
    const char* w1 = first[rand() % 30];
    const char* w2 = second[rand() % 30];
    int num = rand() % 10000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%s%04d", w1, w2, num);
    return std::string(buf);
  }

  bool Initialize() {
    constexpr size_t kPermanentSize = Megabytes(64);
    constexpr size_t kTransientSize = Megabytes(32);
    constexpr size_t kWorkSize      = Megabytes(4);

    u8* perm_mem  = (u8*)malloc(kPermanentSize);
    u8* trans_mem = (u8*)malloc(kTransientSize);
    u8* work_mem  = (u8*)malloc(kWorkSize);
    if (!perm_mem || !trans_mem || !work_mem) {
      free(perm_mem);
      free(trans_mem);
      free(work_mem);
      return false;
    }

    perm_arena  = null::MemoryArena(perm_mem,  kPermanentSize);
    trans_arena = null::MemoryArena(trans_mem, kTransientSize);
    work_arena  = null::MemoryArena(work_mem,  kWorkSize);

    work_queue = new null::WorkQueue(work_arena);
    worker     = new null::Worker(*work_queue);
    worker->Launch();

    perm_global = &perm_arena;

    srand((unsigned)time(nullptr));

    if (!null::g_IOSSettings.username.empty()) {
      snprintf(name, sizeof(name), "%s", null::g_IOSSettings.username.c_str());
    } else {
      std::string gen = GenerateRandomUsername();
      snprintf(name, sizeof(name), "%s", gen.c_str());
      null::g_IOSSettings.SetUsername(gen);
    }
    strcpy(password, kPlayerPassword);

    SetPlatform();
    return true;
  }

  bool JoinZone(int server_index, const char* arena_name) {
    kServerName = kServers[server_index].name;
    kServerIp   = kServers[server_index].server;
    kServerPort = kServers[server_index].port;
    kArenaName  = arena_name;

    perm_arena.Reset();

    kPlayerName = name;

    game = memory_arena_construct_type(&perm_arena, null::Game,
                                        perm_arena, trans_arena, *work_queue,
                                        surface_width, surface_height);
    if (!game->Initialize(g_InputState)) {
      NSLog(@"[nullspace] JoinZone: game->Initialize() failed");
      return false;
    }

    null::ConnectResult result = game->connection.Connect(kServerIp, kServerPort);
    if (result != null::ConnectResult::Success) {
      NSLog(@"[nullspace] JoinZone: Connect() failed, result=%d", (int)result);
      return false;
    }

    screen = GameScreen::Playing;
    game->connection.SendEncryptionRequest(null::g_Settings.encrypt_method);
    return true;
  }

  bool Update() {
    constexpr float kMaxDelta = 1.0f / 20.0f;

    using ms_float = std::chrono::duration<float, std::milli>;
    auto start = std::chrono::high_resolution_clock::now();

    float dt = frame_time / 1000.0f;
    if (dt > kMaxDelta) dt = kMaxDelta;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (screen == GameScreen::Playing) {
      game->connection.Tick();

      if (!game->Update(g_InputState, dt)) {
        glUseProgram(0);
        screen = GameScreen::MainMenu;
        game->Cleanup();
        // Notify ViewController to re-show menu (done by polling iOSIsInGame())
        return true;
      }

      game->Render(dt);
    }
    // (MainMenu rendering is handled by UIKit in ViewController.mm)

    // Present to screen
    glBindRenderbuffer(GL_RENDERBUFFER, g_ColorRB);
    [g_Context presentRenderbuffer:GL_RENDERBUFFER];

    auto end = std::chrono::high_resolution_clock::now();
    frame_time = std::chrono::duration_cast<ms_float>(end - start).count();

    trans_arena.Reset();
    return true;
  }
};

static nullspace_ios_state g_State;

// ── platform functions ────────────────────────────────────────────────────────
static void iOSLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  // stderr shows up directly in `xcrun simctl log stream`
  fprintf(stderr, "[nullspace] %s\n", buf);
  fflush(stderr);
}

static const char* iOSGetStoragePath(null::MemoryArena& arena, const char* path) {
  NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  NSString* docs  = paths.firstObject;
  NSString* full  = [docs stringByAppendingPathComponent:[NSString stringWithUTF8String:path]];
  const char* utf = [full UTF8String];
  char* out = (char*)arena.Allocate(strlen(utf) + 1);
  strcpy(out, utf);
  return out;
}

static u8* iOSLoadAsset(const char* filename, size_t* size) {
  // 1. Try Documents directory first (user-placed or cached files)
  NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
  NSString* docs = paths.firstObject;
  NSString* full = [docs stringByAppendingPathComponent:[NSString stringWithUTF8String:filename]];
  NSData* data = [NSData dataWithContentsOfFile:full];

  if (!data) {
    // 2. Try bundle Resources with full relative path (e.g. "graphics/ships.bm2")
    NSString* nsFilename = [NSString stringWithUTF8String:filename];
    NSString* dir  = [nsFilename stringByDeletingLastPathComponent];
    NSString* base = [[nsFilename lastPathComponent] stringByDeletingPathExtension];
    NSString* ext  = [[nsFilename lastPathComponent] pathExtension];
    NSString* bundlePath = [[NSBundle mainBundle] pathForResource:base ofType:ext
                                                      inDirectory:dir];
    if (bundlePath) data = [NSData dataWithContentsOfFile:bundlePath];
  }

  if (!data) {
    NSLog(@"[nullspace] Asset not found: %s", filename);
    *size = 0;
    return nullptr;
  }

  *size = data.length;
  u8* buf = (u8*)malloc(*size);
  memcpy(buf, data.bytes, *size);
  return buf;
}

static u8* iOSLoadAssetArena(null::MemoryArena& arena, const char* filename, size_t* size) {
  u8* raw = iOSLoadAsset(filename, size);
  if (!raw) return nullptr;
  u8* buf = arena.Allocate(*size);
  memcpy(buf, raw, *size);
  free(raw);
  return buf;
}

static bool iOSCreateFolder(const char* path) {
  NSString* nspath = [NSString stringWithUTF8String:path];
  NSError* err = nil;
  return [[NSFileManager defaultManager] createDirectoryAtPath:nspath
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&err];
}

static void iOSPasteClipboard(char* dest, size_t available_size) {
  UIPasteboard* pb = [UIPasteboard generalPasteboard];
  if (!pb.string) return;
  const char* utf = [pb.string UTF8String];
  for (size_t i = 0; i < available_size - 1 && utf[i] && utf[i] != '\n'; ++i)
    dest[i] = utf[i];
  dest[available_size - 1] = '\0';
}

static unsigned int iOSGetMachineId() {
  NSUUID* uuid = [UIDevice currentDevice].identifierForVendor;
  if (!uuid) return (unsigned int)(rand() % 0x6FFF0000) + 0xFFFF;
  // Hash the UUID string into a 32-bit value
  const char* str = [uuid.UUIDString UTF8String];
  unsigned int h  = 0;
  while (*str) { h = h * 31 + (unsigned char)*str++; }
  return h;
}

static int iOSGetTimeZoneBias() {
  NSTimeZone* tz   = [NSTimeZone localTimeZone];
  // bias in minutes, positive = behind UTC (same convention as Win32)
  return (int)(-(tz.secondsFromGMT / 60));
}

static void SetPlatform() {
  static const null::Platform kIOSPlatform = {
    iOSLog,
    iOSLog,
    iOSGetStoragePath,
    iOSLoadAsset,
    iOSLoadAssetArena,
    iOSCreateFolder,
    iOSPasteClipboard,
    iOSGetMachineId,
    iOSGetTimeZoneBias
  };
  null::platform = kIOSPlatform;
}

// ── OpenGL ES setup ────────────────────────────────────────────────────────────
static bool CreateFramebuffer(CAEAGLLayer* layer, int physical_width, int physical_height) {
  glGenFramebuffers(1, &g_Framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, g_Framebuffer);

  glGenRenderbuffers(1, &g_ColorRB);
  glBindRenderbuffer(GL_RENDERBUFFER, g_ColorRB);
  [g_Context renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_ColorRB);

  // Query the actual renderbuffer dimensions — these come from the layer's contentsScale
  // and bounds, which we've already forced to landscape. Use these as the truth so that
  // the depth buffer and everything else exactly matches the color renderbuffer.
  GLint w = 0, h = 0;
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH,  &w);
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
  if (w == 0 || h == 0) {
    // Fallback to explicit dims if query fails
    w = physical_width;
    h = physical_height;
  }
  // If portrait-shaped (layer wasn't ready), swap to landscape
  if (h > w) { GLint tmp = w; w = h; h = tmp; }
  g_PhysicalWidth  = w;
  g_PhysicalHeight = h;

  glGenRenderbuffers(1, &g_DepthRB);
  glBindRenderbuffer(GL_RENDERBUFFER, g_DepthRB);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_DepthRB);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    NSLog(@"[nullspace] Framebuffer incomplete: 0x%X", status);
    return false;
  }
  return true;
}

static void ConfigureRenderViewport(float screen_scale) {
  int padding = (int)(24.0f * screen_scale + 0.5f);

  g_ViewportX = padding;
  g_ViewportY = padding;
  g_ViewportWidth = g_PhysicalWidth - padding * 2;
  g_ViewportHeight = g_PhysicalHeight - padding * 2;

  if (g_ViewportWidth < 1) {
    g_ViewportX = 0;
    g_ViewportWidth = g_PhysicalWidth;
  }
  if (g_ViewportHeight < 1) {
    g_ViewportY = 0;
    g_ViewportHeight = g_PhysicalHeight;
  }

  glViewport(g_ViewportX, g_ViewportY, g_ViewportWidth, g_ViewportHeight);
}

// ── Public C interface ─────────────────────────────────────────────────────────
void iOSInit(void* eagl_layer, int physical_width, int physical_height, float screen_scale, bool is_tablet) {
  if (g_Initialized) return;

  CAEAGLLayer* layer = (__bridge CAEAGLLayer*)eagl_layer;

  // Create OpenGL ES 3 context
  g_Context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
  if (!g_Context) {
    NSLog(@"[nullspace] Failed to create GLES3 context, falling back to GLES2");
    g_Context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
  }
  [EAGLContext setCurrentContext:g_Context];

  // Load GLAD via dlsym
  if (!gladLoadGLES2Loader((GLADloadproc)ios_gl_get_proc)) {
    NSLog(@"[nullspace] Failed to initialize GLAD");
  }

  if (!CreateFramebuffer(layer, physical_width, physical_height)) return;

  ConfigureRenderViewport(screen_scale);

  int render_w = g_ViewportWidth;
  int render_h = g_ViewportHeight;

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);

  null::InitializeSettings();
  null::g_IOSSettings.Load();

  // Calculate game scale based on device type and screen scale
  // Reference: Original working scale was 3.0 for iPhones
  // This gave good results on iPhone 17 Pro and similar devices
  
  const float kOriginalGameScale = 3.0f;  // Original scale that worked well
  const float kPhoneScaleRatio = 1.0f;    // Phones use original scale
  const float kTabletScaleRatio = 0.67f;  // Tablets scale down to show more content
  
  float game_scale;
  
  if (is_tablet) {
    // iPad: Reduce scale to fit more content on larger screens
    game_scale = kOriginalGameScale * kTabletScaleRatio;
  } else {
    // iPhone: Use original scale that was working
    game_scale = kOriginalGameScale * kPhoneScaleRatio;
  }
  
  g_State.surface_width   = (int)(render_w / game_scale);
  g_State.surface_height  = (int)(render_h / game_scale);
  g_State.physical_width  = render_w;
  g_State.physical_height = render_h;
  g_State.scale           = screen_scale;

  if (NULLSPACE_IOS_DEBUG_LOG) {
    NSLog(@"[nullspace] startup device=%@ physical=%dx%d viewport=%dx%d logical=%dx%d nativeScale=%.1f gameScale=%.2f",
          is_tablet ? @"iPad" : @"iPhone",
          g_PhysicalWidth, g_PhysicalHeight, render_w, render_h,
          g_State.surface_width, g_State.surface_height, screen_scale, game_scale);
  }

  g_State.Initialize();
  g_Initialized = true;
}

void iOSTick(void) {
  if (!g_Initialized) return;
  [EAGLContext setCurrentContext:g_Context];
  glBindFramebuffer(GL_FRAMEBUFFER, g_Framebuffer);
  glViewport(g_ViewportX, g_ViewportY, g_ViewportWidth, g_ViewportHeight);
  g_State.Update();
}

void iOSShutdown(void) {
  if (!g_Initialized) return;
  if (g_Framebuffer)  { glDeleteFramebuffers(1,  &g_Framebuffer);  g_Framebuffer  = 0; }
  if (g_ColorRB)      { glDeleteRenderbuffers(1, &g_ColorRB);      g_ColorRB      = 0; }
  if (g_DepthRB)      { glDeleteRenderbuffers(1, &g_DepthRB);      g_DepthRB      = 0; }
  g_Context    = nil;
  g_Initialized = false;
}

int iOSIsInGame(void) {
  return (g_State.screen == GameScreen::Playing) ? 1 : 0;
}

void iOSJoinZone(int server_index, const char* arena_name) {
  if (server_index < 0 || server_index >= (int)(sizeof(kServers) / sizeof(kServers[0]))) {
    fprintf(stderr, "[nullspace] iOSJoinZone: invalid server index %d\n", server_index);
    return;
  }
  bool ok = g_State.JoinZone(server_index, arena_name);
  if (!ok) {
    fprintf(stderr, "[nullspace] iOSJoinZone failed for server=%s arena=%s\n",
            kServers[server_index].server, arena_name);
    fflush(stderr);
  }
}

const char* iOSGetUsername(void) {
  return g_State.name;
}

void iOSRegenerateUsername(void) {
  std::string gen = g_State.GenerateRandomUsername();
  snprintf(g_State.name, sizeof(g_State.name), "%s", gen.c_str());
  null::g_IOSSettings.SetUsername(gen);
}

// ── Touch input ─────────────────────────────────────────────────────────────

// Mirrors handleInputEvent() in nullspace_android.cpp.
// x/y are physical pixels; screen_width/height are physical screen dimensions.
static void HandleTouchEvent(int flags, float x, float y, long pointer_id,
                              int screen_width, int screen_height) {
  if (!g_Initialized) return;
  null::Game* game = g_State.game;
  if (!game) return;

  null::Player* self = game->player_manager.GetSelf();
  if (!self) return;

  static constexpr int AMOTION_DOWN         = 0;
  static constexpr int AMOTION_UP           = 1;
  static constexpr int AMOTION_MOVE         = 2;
  static constexpr int AMOTION_POINTER_DOWN = 5;

  float viewport_x = x - (float)g_ViewportX;
  float viewport_y = y - (float)g_ViewportY;
  if (viewport_x < 0.0f) viewport_x = 0.0f;
  if (viewport_y < 0.0f) viewport_y = 0.0f;
  if (viewport_x > (float)g_ViewportWidth) viewport_x = (float)g_ViewportWidth;
  if (viewport_y > (float)g_ViewportHeight) viewport_y = (float)g_ViewportHeight;

  float viewport_width = (g_ViewportWidth > 0) ? (float)g_ViewportWidth : (float)screen_width;
  float viewport_height = (g_ViewportHeight > 0) ? (float)g_ViewportHeight : (float)screen_height;

  float nx = (viewport_x / viewport_width)  - 0.5f;
  float ny = (viewport_y / viewport_height) - 0.5f;

  float logical_x = viewport_x * (g_State.surface_width  / viewport_width);
  float logical_y = viewport_y * (g_State.surface_height / viewport_height);
  float lw = (float)g_State.surface_width;
  float lh = (float)g_State.surface_height;

  // ── Slot assignment: left half drives joystick, right half drives fire ─────
  if (flags == AMOTION_DOWN || flags == AMOTION_POINTER_DOWN) {
    if (logical_x < lw * 0.5f) {
      if (ios_input.left_ptr == -1) ios_input.left_ptr = pointer_id;
    } else {
      if (ios_input.right_ptr == -1) ios_input.right_ptr = pointer_id;
    }
  }

  bool owns_left  = (ios_input.left_ptr  == pointer_id);
  bool owns_right = (ios_input.right_ptr == pointer_id);

  // ── Fire / ability buttons (right-slot pointer only) ──────────────────────
  if (self->ship < 8 && owns_right &&
      (flags == AMOTION_MOVE || flags == AMOTION_DOWN || flags == AMOTION_POINTER_DOWN)) {
    float button_size   = 72.0f;
    float button_radius = button_size / 2.0f;
    float button_y      = lh - button_size - 10.0f;
    float button_cy     = button_y + button_radius;
    float gun_x         = lw - (button_size * 2) - 12.0f - 40.0f;
    float bomb_x        = gun_x + button_size + 12.0f;
    float gun_cx        = gun_x  + button_radius;
    float bomb_cx       = bomb_x + button_radius;

    float gdx = logical_x - gun_cx,  gdy = logical_y - button_cy;
    float bdx = logical_x - bomb_cx, bdy = logical_y - button_cy;
    bool gun_pressed  = (gdx*gdx + gdy*gdy <= button_radius*button_radius*1.5f);
    bool bomb_pressed = (bdx*bdx + bdy*bdy <= button_radius*button_radius*1.5f);

    if (flags == AMOTION_DOWN && !ios_input.abilities_triggered) {
      // Right-side toggle icons
      float ri_x       = lw - 26.0f;
      float ri_start_y = lh - 160.0f;
      if (logical_x >= ri_x && logical_y >= ri_start_y) {
        int ri = (int)((logical_y - ri_start_y) / 25.0f);
        if (ri == 0) {
          if (g_InputState.action_callback)
            g_InputState.action_callback(g_InputState.user, null::InputAction::Multifire);
          ios_input.abilities_triggered = true;
        } else if (ri == 1) {
          ios_input.mine_mode_active = !ios_input.mine_mode_active;
          null::g_MobileMineMode = ios_input.mine_mode_active;
          if (ios_input.mine_mode_active) game->sound_system.Play(null::AudioType::Mine1);
          else                            game->sound_system.Play(null::AudioType::ToggleOff);
          ios_input.abilities_triggered = true;
        } else if (ri >= 2 && ri <= 5) {
          null::InputAction act;
          switch (ri) {
            case 2: act = null::InputAction::Stealth;   break;
            case 3: act = null::InputAction::Cloak;     break;
            case 4: act = null::InputAction::XRadar;    break;
            case 5: act = null::InputAction::Antiwarp;  break;
            default: act = null::InputAction::Stealth;  break;
          }
          if (g_InputState.action_callback)
            g_InputState.action_callback(g_InputState.user, act);
          ios_input.abilities_triggered = true;
        }
      }
    }

    g_InputState.SetAction(null::InputAction::Bullet, gun_pressed);
    if (bomb_pressed) {
      g_InputState.SetAction(null::InputAction::Bomb, !ios_input.mine_mode_active);
      g_InputState.SetAction(null::InputAction::Mine, ios_input.mine_mode_active);
    } else {
      g_InputState.SetAction(null::InputAction::Bomb, false);
      g_InputState.SetAction(null::InputAction::Mine, false);
    }
  }

  // ── Left-side ability icons (left-slot pointer only) ──────────────────────
  if (self->ship < 8 && owns_left &&
      flags == AMOTION_DOWN && !ios_input.abilities_triggered) {
    float item_stack_h  = 175.0f;
    float items_start_y = lh - item_stack_h - 10.0f;
    float item_w = 26.0f, item_h = 25.0f;
    if (logical_x >= 0 && logical_x <= item_w && logical_y >= items_start_y) {
      int idx = (int)((logical_y - items_start_y) / item_h);
      if (idx >= 0 && idx < 7) {
        null::InputAction act;
        switch (idx) {
          case 0: act = null::InputAction::Burst;  break;
          case 1: act = null::InputAction::Repel;  break;
          case 2: act = null::InputAction::Decoy;  break;
          case 3: act = null::InputAction::Thor;   break;
          case 4: act = null::InputAction::Brick;  break;
          case 5: act = null::InputAction::Rocket; break;
          case 6:
            act = (game->ship_controller.ship.portal_time > 0.0f)
                  ? null::InputAction::Warp
                  : null::InputAction::Portal;
            break;
          default: act = null::InputAction::Burst; break;
        }
        g_InputState.SetAction(act, true);
        ios_input.abilities_triggered = true;
      }
    }
  }

  if (flags == AMOTION_DOWN || flags == AMOTION_POINTER_DOWN) {
    ios_input.touch_start_time  = now_ns();
    ios_input.touch_start_x     = x;
    ios_input.touch_start_y     = y;
    ios_input.long_press_triggered = false;

  } else if (flags == AMOTION_UP) {
    // Release the slot owned by this pointer
    if (owns_left) {
      ios_input.left_ptr        = -1;
      ios_input.anchored        = false;
      ios_input.joystick_active = false;
      ios_input.joystick_x      = ios_input.joystick_y = 0;
    }
    if (owns_right) {
      ios_input.right_ptr           = -1;
      ios_input.abilities_triggered = false;
    }

    if (self->ship < 8) {
      if (owns_left) {
        g_InputState.SetAction(null::InputAction::Forward,     false);
        g_InputState.SetAction(null::InputAction::Left,        false);
        g_InputState.SetAction(null::InputAction::Right,       false);
        g_InputState.SetAction(null::InputAction::Afterburner, false);
      }
      if (owns_right) {
        g_InputState.SetAction(null::InputAction::Bullet,      false);
        g_InputState.SetAction(null::InputAction::Bomb,        false);
        g_InputState.SetAction(null::InputAction::Mine,        false);
      }
      // One-shot ability actions cleared on any finger up
      if (owns_left || owns_right) {
        g_InputState.SetAction(null::InputAction::Burst,       false);
        g_InputState.SetAction(null::InputAction::Repel,       false);
        g_InputState.SetAction(null::InputAction::Decoy,       false);
        g_InputState.SetAction(null::InputAction::Thor,        false);
        g_InputState.SetAction(null::InputAction::Brick,       false);
        g_InputState.SetAction(null::InputAction::Rocket,      false);
        g_InputState.SetAction(null::InputAction::Portal,      false);
      }
    }

    int64_t dur = now_ns() - ios_input.touch_start_time;
    float dx_tap = x - ios_input.touch_start_x;
    float dy_tap = y - ios_input.touch_start_y;
    bool  is_tap = (dx_tap*dx_tap + dy_tap*dy_tap) < 400.0f;

    if (is_tap && game->onboarding.OnTap()) {
      // Onboarding is a modal overlay; consume taps before game/menu controls.
    } else if (game->show_all_statboxes && is_tap) {
      game->show_all_statboxes = false;
    } else if (game->menu_open && is_tap) {
      // Full screen menu (100% of screen)
      float menu_w = lw, menu_h = lh;
      float menu_x = 0.0f, menu_y = 0.0f;

      // Left side - ship selection grid
      float gsx = 20.0f, gsy = 78.0f;
      float cw = 110.0f, ch = 70.0f, gs = 8.0f;

      // Right column - Scoreboard, Help, Quit
      float rbw = 140.0f, rbh = 35.0f;
      float button_margin = 20.0f;
      float rcx = menu_w - rbw - button_margin, rsy = 30.0f;

      bool handled = false;

      // Check ship grid (left side)
      if (logical_x >= gsx && logical_x <= gsx + 3*(cw+gs) &&
          logical_y >= gsy && logical_y <= gsy + 3*(ch+gs)) {
        int col = (int)((logical_x - gsx) / (cw+gs));
        int row = (int)((logical_y - gsy) / (ch+gs));
        if (col>=0 && col<3 && row>=0 && row<3) {
          int si = row*3 + col;
          if (si == 8)      g_InputState.OnCharacter('s');
          else if (si < 8)  g_InputState.OnCharacter('1' + si);
          handled = true;
        }
      }
      // Check right column buttons (Scoreboard, Help, Quit)
      if (!handled &&
          logical_x >= rcx && logical_x <= rcx + rbw &&
          logical_y >= rsy && logical_y <= rsy + 3*(rbh+8.0f)) {
        int bi = (int)((logical_y - rsy) / (rbh + 8.0f));
        if      (bi == 0) { game->show_all_statboxes = !game->show_all_statboxes; game->menu_open = false; }
        else if (bi == 1) { game->onboarding.Show(); game->menu_open = false; }
        else if (bi == 2) { g_InputState.OnCharacter('q'); }
        handled = true;
      }
      // Check CLOSE button at bottom right
      float close_x = rcx;
      float close_y = menu_h - rbh - button_margin;
      if (!handled &&
          logical_x >= close_x && logical_x <= close_x + rbw &&
          logical_y >= close_y && logical_y <= close_y + rbh) {
        g_InputState.OnCharacter(NULLSPACE_KEY_ESCAPE);
        handled = true;
      }
    } else {
      bool is_top_ui = (logical_y < 80.0f) && is_tap;
      if (is_top_ui && dur < LONG_PRESS_TIMEOUT)
        g_InputState.OnCharacter(NULLSPACE_KEY_ESCAPE);
    }

    ios_input.last_tap_time = now_ns();
    ios_input.last_tap_x    = x;
    ios_input.last_tap_y    = y;

  } else {
    // MOVE
    if (self->ship == 8) {
      // Spectate camera: any pointer pans (no slot restriction in spec mode)
      if (!ios_input.anchored) {
        ios_input.world_x = self->position.x;
        ios_input.world_y = self->position.y;
        ios_input.nx      = nx;
        ios_input.ny      = ny;
        ios_input.anchored= true;
      }
      float ox = (ios_input.nx - nx) * (game->ui_camera.surface_dim.x * (1.0f/16.0f));
      float oy = (ios_input.ny - ny) * (game->ui_camera.surface_dim.y * (1.0f/16.0f));
      self->position.x = ios_input.world_x + ox;
      self->position.y = ios_input.world_y + oy;
      if (fabsf(ox) > 1.0f || fabsf(oy) > 1.0f)
        game->specview.spectate_id = null::kInvalidSpectateId;

      int64_t now = now_ns();
      if (now - ios_input.last_tap_time <= DOUBLE_TAP_TIMEOUT) {
        float dtx = x - ios_input.last_tap_x;
        float dty = y - ios_input.last_tap_y;
        if (dtx*dtx + dty*dty < DOUBLE_TAP_SLOP*DOUBLE_TAP_SLOP) {
          null::Vector2f wp(self->position.x + nx*(game->ui_camera.surface_dim.x*(1.0f/16.0f)),
                            self->position.y + ny*(game->ui_camera.surface_dim.y*(1.0f/16.0f)));
          null::Player* closest = nullptr;
          float closest_d = 10000.0f;
          for (size_t i = 0; i < game->player_manager.player_count; ++i) {
            null::Player* p = game->player_manager.players + i;
            if (p->ship == 8) continue;
            float d = p->position.DistanceSq(wp);
            if (d < closest_d) { closest_d = d; closest = p; }
          }
          if (closest && closest_d <= 16.0f) {
            game->specview.spectate_id        = closest->id;
            game->specview.spectate_frequency = closest->frequency;
          }
        }
      }
    } else if (owns_left) {
      // Joystick movement: only the left-slot pointer drives the dpad
      float dpad_size   = 140.0f;
      float dpad_radius = dpad_size / 2.0f;
      float dpad_cx     = 50.0f + dpad_radius;
      float btn_size    = 72.0f;
      float btn_y       = lh - btn_size - 10.0f;
      float dpad_cy     = btn_y + btn_size/2.0f - 40.0f - (lh * 0.05f);

      float dx  = logical_x - dpad_cx;
      float dy  = logical_y - dpad_cy;
      float dist= sqrtf(dx*dx + dy*dy);

      if (dist < dpad_radius * 3.0f) {
        ios_input.joystick_active = (dist > 15.0f);
        if (ios_input.joystick_active) {
          float angle = atan2f(dy, dx) + (3.14159265f / 2.0f);
          float desired = angle / (2.0f * 3.14159265f);
          if (desired < 0) desired += 1.0f;

          float diff = desired - self->orientation;
          if (diff > 0.5f)  diff -= 1.0f;
          if (diff < -0.5f) diff += 1.0f;

          const float kThresh = 0.02f;
          if (diff < -kThresh) {
            g_InputState.SetAction(null::InputAction::Left,  true);
            g_InputState.SetAction(null::InputAction::Right, false);
          } else if (diff > kThresh) {
            g_InputState.SetAction(null::InputAction::Right, true);
            g_InputState.SetAction(null::InputAction::Left,  false);
          } else {
            g_InputState.SetAction(null::InputAction::Left,  false);
            g_InputState.SetAction(null::InputAction::Right, false);
          }
          g_InputState.SetAction(null::InputAction::Afterburner, dist > dpad_radius);
          g_InputState.SetAction(null::InputAction::Forward, true);
        } else {
          g_InputState.SetAction(null::InputAction::Forward,     false);
          g_InputState.SetAction(null::InputAction::Left,        false);
          g_InputState.SetAction(null::InputAction::Right,       false);
          g_InputState.SetAction(null::InputAction::Afterburner, false);
        }
      } else {
        ios_input.joystick_active = false;
        g_InputState.SetAction(null::InputAction::Forward,     false);
        g_InputState.SetAction(null::InputAction::Left,        false);
        g_InputState.SetAction(null::InputAction::Right,       false);
        g_InputState.SetAction(null::InputAction::Afterburner, false);
      }
    }
  }
}

void iOSTouchBegan(float x, float y, long pointer_id, int sw, int sh) {
  HandleTouchEvent(0 /*DOWN*/, x, y, pointer_id, sw, sh);
}
void iOSTouchMoved(float x, float y, long pointer_id, int sw, int sh) {
  HandleTouchEvent(2 /*MOVE*/, x, y, pointer_id, sw, sh);
}
void iOSTouchEnded(float x, float y, long pointer_id, int sw, int sh) {
  HandleTouchEvent(1 /*UP*/, x, y, pointer_id, sw, sh);
}
void iOSTouchCancelled(int sw, int sh) {
  // Treat as all-up — clear all state
  if (!g_Initialized) return;
  ios_input.left_ptr        = -1;
  ios_input.right_ptr       = -1;
  ios_input.anchored        = false;
  ios_input.joystick_active = false;
  ios_input.abilities_triggered = false;
  ios_input.mine_mode_active    = false;
  null::g_MobileMineMode           = false;
  g_InputState.SetAction(null::InputAction::Forward,     false);
  g_InputState.SetAction(null::InputAction::Left,        false);
  g_InputState.SetAction(null::InputAction::Right,       false);
  g_InputState.SetAction(null::InputAction::Afterburner, false);
  g_InputState.SetAction(null::InputAction::Bullet,      false);
  g_InputState.SetAction(null::InputAction::Bomb,        false);
  g_InputState.SetAction(null::InputAction::Mine,        false);
}
