#include "imgui.h"
//
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"
//
#include <EGL/egl.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <null/Game.h>
#include <null/Memory.h>
#include <null/Platform.h>
#include <null/net/Connection.h>
#include <null/render/Image.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>

static EGLDisplay g_EglDisplay = EGL_NO_DISPLAY;
static EGLSurface g_EglSurface = EGL_NO_SURFACE;
static EGLContext g_EglContext = EGL_NO_CONTEXT;
static EGLConfig g_EglConfig = nullptr;
static EGLint g_EglFormat = 0;
static int g_SurfaceBufferWidth = 0;   // width passed to ANativeWindow_setBuffersGeometry
static int g_SurfaceBufferHeight = 0;  // height passed to ANativeWindow_setBuffersGeometry
static int g_PhysicalWidth = 0;        // actual EGL surface width after creation
static int g_PhysicalHeight = 0;
static struct android_app* g_App = NULL;
static bool g_Initialized = false;
static bool g_EglContextCreated = false;  // true once display/context/resources are up
static char g_LogTag[] = "nullspace";

// Login screen background sprites
static GLuint g_ShipTextureId = 0;
static int g_ShipWidth = 0;
static int g_ShipHeight = 0;
static GLuint g_AsteroidTextureId = 0;
static int g_AsteroidWidth = 0;
static int g_AsteroidHeight = 0;
static bool g_BackgroundLoadAttempted = false;

static struct {
  float world_x;
  float world_y;
  float nx;
  float ny;
  bool anchored;

  int64_t last_tap_time;
  float last_tap_x;
  float last_tap_y;
  
  int64_t touch_start_time;
  float touch_start_x;
  float touch_start_y;
  bool long_press_triggered;
  
  // Multi-touch support
  int32_t flying_pointer_id = -1;  // -1 if no flying touch active
  int32_t gun_pointer_id = -1;     // -1 if no gun button pressed
  int32_t bomb_pointer_id = -1;    // -1 if no bomb button pressed
  
  // Track ability triggers to prevent repeat firing
  bool abilities_triggered = false;
  
  // Track mine mode toggle
  bool mine_mode_active = false;
  
  // Virtual joystick
  float joystick_x = 0;  // Current joystick displacement X (-1 to 1)
  float joystick_y = 0;  // Current joystick displacement Y (-1 to 1)
  bool joystick_active = false;
} android_input;
const int32_t DOUBLE_TAP_TIMEOUT = 300 * 1000000;
const int32_t LONG_PRESS_TIMEOUT = 800 * 1000000; // 800ms for long press
const int32_t DOUBLE_TAP_SLOP = 100;

// Forward declarations of helper functions
static int ShowSoftKeyboardInput();
static int PollUnicodeChars();
static void SetPlatform();
void shutdown();
static void destroySurfaceOnly();
static bool ensureSurface();

namespace null {

// Global mine mode state accessible from rendering code
bool g_AndroidMineMode = false;

GameSettings g_Settings;

void InitializeSettings() {
  g_Settings.vsync = true;
  g_Settings.window_type = WindowType::Windowed;
  g_Settings.render_stars = true;

  g_Settings.encrypt_method = EncryptMethod::Subspace;

  g_Settings.sound_enabled = true;
  g_Settings.sound_volume = 0.25f;
  g_Settings.sound_radius_increase = 10.0f;

  g_Settings.notify_max_prizes = false;
  g_Settings.target_bounty = 20;
}

const char* kPlayerName = "null space";
const char* kPlayerPassword = "none";

struct ServerInfo {
  const char* name;
  const char* server;
  u16 port;
};

ServerInfo kServers[] = {
    {"emulator", "10.0.2.2", 5000},
    {"Null Orbit", "api.null-orbit.com", 5000},
    {"SSCE Hyperspace", "162.248.95.143", 5005},
    {"SSCJ Devastation", "69.164.220.203", 7022},
    {"SSCJ MetalGear CTF", "69.164.220.203", 14000},
    {"SSCU Extreme Games", "208.118.63.35", 7900},
};

constexpr size_t kServerIndex = 0;

static_assert(kServerIndex < NULLSPACE_ARRAY_SIZE(kServers), "Bad server index");

const char* kServerName = kServers[kServerIndex].name;
const char* kServerIp = kServers[kServerIndex].server;
u16 kServerPort = kServers[kServerIndex].port;

InputState g_InputState;
enum class GameScreen { MainMenu, Playing };

MemoryArena* perm_global = nullptr;

// TODO: Merge this with existing main code
struct nullspace {
  MemoryArena perm_arena;
  MemoryArena trans_arena;
  MemoryArena work_arena;
  WorkQueue* work_queue;
  Worker* worker;
  Game* game = nullptr;
  int surface_width = 0;  // Logical dimensions (DPI-scaled)
  int surface_height = 0;
  int physical_width = 0;  // Physical screen dimensions
  int physical_height = 0;
  char name[20];
  char password[20];
  GameScreen screen = GameScreen::MainMenu;
  float frame_time = 0.0f;
  float scale = 1.0f;

  size_t selected_zone_index = 0;

  bool Initialize() {
    constexpr size_t kPermanentSize = Megabytes(64);
    constexpr size_t kTransientSize = Megabytes(32);
    constexpr size_t kWorkSize = Megabytes(4);

    u8* perm_memory = (u8*)malloc(kPermanentSize);
    u8* trans_memory = (u8*)malloc(kTransientSize);
    u8* work_memory = (u8*)malloc(kWorkSize);

    if (!perm_memory || !trans_memory || !work_memory) {
      fprintf(stderr, "Failed to allocate memory.\n");
      return false;
    }

    perm_arena = MemoryArena(perm_memory, kPermanentSize);
    trans_arena = MemoryArena(trans_memory, kTransientSize);
    work_arena = MemoryArena(work_memory, kWorkSize);

    work_queue = new WorkQueue(work_arena);
    worker = new Worker(*work_queue);
    worker->Launch();

    perm_global = &perm_arena;

    // Generate random username: First+Second+4digits
    // First word — evocative space/combat words (mix of nouns + adjectives, all work as prefix)
    static const char* first[] = {
        "Void", "Nova", "Storm", "Shadow", "Frost", "Flame", "Thunder", "Plasma",
        "Eclipse", "Meteor", "Nebula", "Ghost", "Viper", "Titan", "Phoenix",
        "Raven", "Wolf", "Hawk", "Comet", "Pulsar", "Photon", "Zenith",
        "Apex", "Vector", "Neutron", "Crimson", "Azure", "Iron", "Dark", "Solar"
    };
    // Second word — clear action words, all unambiguously verb-like
    static const char* second[] = {
        "Strike", "Surge", "Blast", "Rush", "Pierce", "Crash", "Dive", "Soar",
        "Bolt", "Slash", "Burn", "Shock", "Dash", "Rip", "Tear", "Hunt",
        "Forge", "Break", "Burst", "Flash", "Drift", "Glide", "Freeze", "Shift",
        "Spark", "Pulse", "Fire", "Charge", "Smash", "Raze"
    };
    
    srand(static_cast<unsigned>(time(nullptr)));
    const char* word1 = first[rand() % (sizeof(first) / sizeof(first[0]))];
    const char* word2 = second[rand() % (sizeof(second) / sizeof(second[0]))];
    int random_number = rand() % 10000;
    snprintf(name, sizeof(name), "%s%s%04d", word1, word2, random_number);
    strcpy(password, kPlayerPassword);

    SetPlatform();

    return true;
  }

  bool JoinZone(size_t selected_index) {
    kServerName = kServers[selected_index].name;
    kServerIp = kServers[selected_index].server;
    kServerPort = kServers[selected_index].port;

    perm_arena.Reset();

    kPlayerName = name;
    kPlayerPassword = password;

    game = memory_arena_construct_type(&perm_arena, Game, perm_arena, trans_arena, *work_queue, surface_width,
                                       surface_height);

    if (!game->Initialize(g_InputState)) {
      // TODO: Error pop up
      __android_log_print(ANDROID_LOG_DEBUG, g_LogTag, "Failed to create game.");
      return false;
    }

    null::ConnectResult result = game->connection.Connect(kServerIp, kServerPort);

    if (result != null::ConnectResult::Success) {
      // TODO: Error pop up
      __android_log_print(ANDROID_LOG_DEBUG, g_LogTag, "Failed to connect. Error: %d.", (int)result);
      return false;
    }

    screen = GameScreen::Playing;

    game->connection.SendEncryptionRequest(g_Settings.encrypt_method);

    return true;
  }

  bool HandleMainMenu() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    PollUnicodeChars();

    ImGuiIO& io = ImGui::GetIO();

    // Use physical dimensions for ImGui framebuffer scaling
    io.DisplayFramebufferScale.x = physical_width / io.DisplaySize.x;
    io.DisplayFramebufferScale.y = physical_height / io.DisplaySize.y;

    // Dark background similar to loading screen
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.05f, 0.95f);
    ImGui::GetStyle().Colors[ImGuiCol_Button] = ImVec4(0.259f, 0.259f, 0.420f, 1.0f);  // Dark blue from palette
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = ImVec4(0.388f, 0.388f, 0.580f, 1.0f);  // Lighter blue
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = ImVec4(0.388f, 0.388f, 1.0f, 0.8f);  // Bright blue on click
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White button text

    // Full screen window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoScrollbar;
    
    if (ImGui::Begin("LoginScreen", nullptr, window_flags)) {
      float center_x = io.DisplaySize.x * 0.5f;
      float center_y = io.DisplaySize.y * 0.5f;
      
      // Load background sprites on first render (after platform is set up)
      if (!g_BackgroundLoadAttempted) {
        g_BackgroundLoadAttempted = true;
        __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Loading background sprites...");
        
        // Load ships
        unsigned char* ship_image = null::ImageLoad("graphics/ships.bm2", &g_ShipWidth, &g_ShipHeight);
        if (ship_image) {
          glGenTextures(1, &g_ShipTextureId);
          glBindTexture(GL_TEXTURE_2D, g_ShipTextureId);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_ShipWidth, g_ShipHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, ship_image);
          null::ImageFree(ship_image);
          __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Loaded ships: %dx%d", g_ShipWidth, g_ShipHeight);
        }
        
        // Load asteroids (using over2 - large brown asteroid from map tiles)
        unsigned char* asteroid_image = null::ImageLoad("graphics/over2.bm2", &g_AsteroidWidth, &g_AsteroidHeight);
        if (asteroid_image) {
          glGenTextures(1, &g_AsteroidTextureId);
          glBindTexture(GL_TEXTURE_2D, g_AsteroidTextureId);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_AsteroidWidth, g_AsteroidHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, asteroid_image);
          null::ImageFree(asteroid_image);
          __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Loaded asteroids: %dx%d", g_AsteroidWidth, g_AsteroidHeight);
        }
      }
      
      // Draw background sprites FIRST (below all UI elements)
      // Ships first (furthest back), then asteroids, then UI on top
      
      // Draw asteroids scattered randomly across the screen
      if (g_AsteroidTextureId != 0) {
        // Generate random positions ONCE and store them
        struct AsteroidPosition {
          float x, y, scale;
        };
        static AsteroidPosition asteroid_positions[150];  // Very dense scatter
        static bool positions_generated = false;
        
        if (!positions_generated) {
          srand(time(NULL));
          int count = 0;
          int max_attempts = 800;  // Prevent infinite loop
          
          while (count < 150 && max_attempts > 0) {
            max_attempts--;
            
            // Generate random position
            float x = 50 + (rand() % (int)(io.DisplaySize.x - 100));
            float y = 150 + (rand() % (int)(io.DisplaySize.y - 200));  // Below title
            float scale = 1.2f + (rand() % 4) * 0.3f;
            float size = 32.0f * scale;
            
            // Check for overlap with existing asteroids
            bool overlaps = false;
            float min_distance = size * 0.8f;  // Minimum distance between centers
            
            for (int j = 0; j < count; j++) {
              float dx = x - asteroid_positions[j].x;
              float dy = y - asteroid_positions[j].y;
              float distance = sqrtf(dx * dx + dy * dy);
              
              if (distance < min_distance) {
                overlaps = true;
                break;
              }
            }
            
            // Only add if no overlap
            if (!overlaps) {
              asteroid_positions[count].x = x;
              asteroid_positions[count].y = y;
              asteroid_positions[count].scale = scale;
              count++;
            }
          }
          positions_generated = true;
        }
        
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);  // 60% opacity
        
        // over2.bm2 is 320x96 with 32x32 sprites = 10 columns x 3 rows = 30 frames
        float sprite_size = 32.0f;
        float uv_width = sprite_size / (float)g_AsteroidWidth;   // 32/320 = 0.1
        float uv_height = sprite_size / (float)g_AsteroidHeight; // 32/96 = 0.333...
        
        // Animate: cycle through all 30 frames over 1.5 seconds
        constexpr float kAsteroidDuration = 1.5f;
        constexpr int kFrameCount = 30;  // 10 columns x 3 rows
        constexpr int kColumns = 10;
        
        float time = ImGui::GetTime();
        float anim_progress = fmodf(time, kAsteroidDuration) / kAsteroidDuration;  // 0-1
        int frame_index = (int)(anim_progress * kFrameCount) % kFrameCount;
        
        // Calculate UV for current frame (frames go left-to-right, top-to-bottom)
        int frame_col = frame_index % kColumns;
        int frame_row = frame_index / kColumns;
        float uv_x = frame_col * uv_width;
        float uv_y = frame_row * uv_height;
        ImVec2 uv0(uv_x, uv_y);
        ImVec2 uv1(uv_x + uv_width, uv_y + uv_height);
        
        // Draw all asteroids at their static positions
        for (int i = 0; i < 150; i++) {
          float display_size = sprite_size * asteroid_positions[i].scale;
          ImVec2 pos(asteroid_positions[i].x - display_size * 0.5f, asteroid_positions[i].y - display_size * 0.5f);
          
          ImGui::SetCursorPos(pos);
          ImGui::Image((ImTextureID)(intptr_t)g_AsteroidTextureId, ImVec2(display_size, display_size), uv0, uv1);
        }
        ImGui::PopStyleVar();
      }
      
      // Draw ships in a centered row directly below the NULLORBIT title
      if (g_ShipTextureId != 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);  // 60% opacity
        float ship_size = 36.0f;
        float uv_width = ship_size / (float)g_ShipWidth;
        float uv_height = ship_size / (float)g_ShipHeight;
        
        // Calculate rows per ship
        int total_rows = g_ShipHeight / (int)ship_size;  // 32
        int rows_per_ship = total_rows / 8;  // 4
        
        // Position ships in a centered horizontal row
        float ship_display_size = ship_size * 3.5f;
        float spacing = ship_display_size + 20.0f;  // Space between ships
        float total_width = (8 * ship_display_size) + (7 * 20.0f);  // Total width of all ships
        float start_x = (io.DisplaySize.x - total_width) * 0.5f;  // Center horizontally
        float y_pos = 180.0f + (ship_display_size * 0.5f) + 30.0f;  // Below NULLORBIT title, moved down by half height + padding
        
        // Draw all 8 ships in order
        for (int i = 0; i < 8; i++) {
          float x = start_x + i * spacing;
          ImVec2 pos(x, y_pos);
          
          // Calculate UV for this ship type
          float uv_y_start = (i * rows_per_ship) * uv_height;
          float uv_y_end = uv_y_start + uv_height;
          ImVec2 uv0(0.0f, uv_y_start);
          ImVec2 uv1(uv_width, uv_y_end);
          
          ImGui::SetCursorPos(pos);
          ImGui::Image((ImTextureID)(intptr_t)g_ShipTextureId, ImVec2(ship_display_size, ship_display_size), uv0, uv1);
        }
        ImGui::PopStyleVar();
      }
      

      
      // NOW draw UI elements on top
      // Title: NULLORBIT at top (bigger) - Subspace bright blue
      ImGui::SetCursorPosY(60);
      ImGui::SetWindowFontScale(5.0f);
      float title_width = ImGui::CalcTextSize("NULLORBIT").x;
      ImGui::SetCursorPosX(center_x - title_width * 0.5f);
      ImGui::TextColored(ImVec4(0.388f, 0.388f, 1.0f, 1.0f), "NULLORBIT");
      ImGui::SetWindowFontScale(1.0f);
      
      ImGui::Dummy(ImVec2(0, 80));
      
      // Layout: Username on left, Buttons spanning most of screen width
      float left_start = 50;
      float button_margin = 40;
      float button_width = io.DisplaySize.x - (button_margin * 2);  // Almost full width
      float button_height = 170;  // Slightly taller
      
      // Left side: Display username
      ImGui::SetCursorPosX(left_start);
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextColored(ImVec4(0.937f, 0.937f, 1.0f, 1.0f), "Player:");  // Off-white
      ImGui::SetWindowFontScale(1.0f);
      
      ImGui::SetCursorPosX(left_start);
      ImGui::SetWindowFontScale(2.0f);
      ImGui::TextColored(ImVec4(0.451f, 1.0f, 0.388f, 1.0f), "%s", name);  // Neon green from game
      ImGui::SetWindowFontScale(1.0f);
      
      ImGui::Dummy(ImVec2(0, 80));
      
      // "Select an arena:" label
      ImGui::SetCursorPosX(left_start);
      ImGui::SetWindowFontScale(1.5f);
      ImGui::TextColored(ImVec4(0.937f, 0.937f, 1.0f, 1.0f), "Select an arena:");  // Off-white
      ImGui::SetWindowFontScale(1.0f);
      
      ImGui::Dummy(ImVec2(0, 50));
      
      // Trench Wars button
      ImGui::SetCursorPosX(button_margin);
      
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
      
      // Trench Wars button - text centered in button
      ImGui::SetWindowFontScale(1.8f);
      if (ImGui::Button("Trench Wars: Capture the Flag", ImVec2(button_width, button_height))) {
        selected_zone_index = 1;  // "local" server
        JoinZone(selected_zone_index);
      }
      ImGui::SetWindowFontScale(1.0f);
      
      // Emulator button (only show in debug builds) - at bottom of screen
      #ifdef NDEBUG
        // Release build - check if running on emulator
        bool show_emulator = false;
      #else
        // Debug build - always show
        bool show_emulator = true;
      #endif
      
      if (show_emulator) {
        float emulator_button_y = io.DisplaySize.y - (button_height * 0.6f) - 30;
        ImGui::SetCursorPos(ImVec2(button_margin, emulator_button_y));
        ImGui::SetWindowFontScale(1.5f);
        if (ImGui::Button("Emulator (Test)", ImVec2(button_width, button_height * 0.6f))) {
          selected_zone_index = 0;  // "emulator" server
          JoinZone(selected_zone_index);
        }
        ImGui::SetWindowFontScale(1.0f);
      }
      
      ImGui::PopStyleVar();
      
      ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return true;
  }

  bool Update() {
    constexpr float kMaxDelta = 1.0f / 20.0f;

    using ms_float = std::chrono::duration<float, std::milli>;
    auto start = std::chrono::high_resolution_clock::now();

    float dt = frame_time / 1000.0f;

    // Cap dt so window movement doesn't cause large updates
    if (dt > kMaxDelta) {
      dt = kMaxDelta;
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (screen == GameScreen::MainMenu) {
      if (!HandleMainMenu()) {
        return false;
      }
    } else {
      game->connection.Tick();

      if (!game->Update(g_InputState, dt)) {
        // Switch out of any active shaders so they can be cleaned up.
        glUseProgram(0);
        screen = GameScreen::MainMenu;
        game->Cleanup();
        return true;
      }

      game->Render(dt);
    }

    EGLBoolean swapResult = eglSwapBuffers(g_EglDisplay, g_EglSurface);
    if (swapResult == EGL_FALSE) {
      EGLint err = eglGetError();
      __android_log_print(ANDROID_LOG_WARN, g_LogTag, "eglSwapBuffers failed: 0x%x — destroying surface", err);
      if (err == EGL_BAD_SURFACE || err == EGL_CONTEXT_LOST || err == EGL_BAD_NATIVE_WINDOW) {
        destroySurfaceOnly();
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    frame_time = std::chrono::duration_cast<ms_float>(end - start).count();

    trans_arena.Reset();
    return true;
  }
};

static nullspace g_nullspace;

}  // namespace null

void init(struct android_app* app) {
  if (g_Initialized) return;

  __android_log_print(ANDROID_LOG_INFO, g_LogTag, "%s", "Initializing app.");

  void* output = nullptr;

  null::InitializeSettings();

  g_App = app;
  ANativeWindow_acquire(g_App->window);

  int width = (int)(ANativeWindow_getWidth(g_App->window) * 0.6f);
  int height = (int)(ANativeWindow_getHeight(g_App->window) * 0.6f);

  // Initialize EGL
  // This is mostly boilerplate code for EGL...
  {
    g_EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_EglDisplay == EGL_NO_DISPLAY)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s",
                          "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");

    if (eglInitialize(g_EglDisplay, 0, 0) != EGL_TRUE)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglInitialize() returned with an error");

    const EGLint egl_attributes[] = {
        EGL_BLUE_SIZE,  8,       EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT, EGL_NONE};
    EGLint num_configs = 0;
    if (eglChooseConfig(g_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned with an error");
    if (num_configs == 0)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned 0 matching config");

    // Get the first matching config
    EGLConfig egl_config;
    eglChooseConfig(g_EglDisplay, egl_attributes, &egl_config, 1, &num_configs);
    eglGetConfigAttrib(g_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &g_EglFormat);
    g_SurfaceBufferWidth = width;
    g_SurfaceBufferHeight = height;
    ANativeWindow_setBuffersGeometry(g_App->window, width, height, g_EglFormat);

    const EGLint egl_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_EglContext = eglCreateContext(g_EglDisplay, egl_config, EGL_NO_CONTEXT, egl_context_attributes);

    if (g_EglContext == EGL_NO_CONTEXT)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglCreateContext() returned EGL_NO_CONTEXT");

    // Store config globally so we can recreate surface later without full reinit
    g_EglConfig = egl_config;
    g_EglContextCreated = true;

    g_EglSurface = eglCreateWindowSurface(g_EglDisplay, g_EglConfig, g_App->window, NULL);
    if (g_EglSurface == EGL_NO_SURFACE)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "eglCreateWindowSurface failed: 0x%x", eglGetError());

    if (eglMakeCurrent(g_EglDisplay, g_EglSurface, g_EglSurface, g_EglContext) != EGL_TRUE)
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "eglMakeCurrent failed: 0x%x", eglGetError());

    if (!gladLoadGLES2Loader((GLADloadproc)eglGetProcAddress)) {
      __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "Failed to initialize glad loader.");
    }

    eglSwapInterval(g_EglDisplay, null::g_Settings.vsync);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
  }

  int surface_width, surface_height;
  eglQuerySurface(g_EglDisplay, g_EglSurface, EGL_WIDTH, &surface_width);
  eglQuerySurface(g_EglDisplay, g_EglSurface, EGL_HEIGHT, &surface_height);
  g_PhysicalWidth = surface_width;
  g_PhysicalHeight = surface_height;

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();

  // Disable loading/saving of .ini file from disk.
  // FIXME: Consider using LoadIniSettingsFromMemory() / SaveIniSettingsToMemory() to save in appropriate location for
  // Android.
  io.IniFilename = NULL;

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();
  // ImGui::StyleColorsClassic();

  // Setup Platform/Renderer backends
  ImGui_ImplAndroid_Init(g_App->window);
  ImGui_ImplOpenGL3_Init("#version 300 es");

  // Calculate scale based on display density for better DPI awareness
  // Base scale of 3.0f works well for 420 dpi displays (like emulator)
  // Scale proportionally for higher/lower DPI displays
  AConfiguration* config = AConfiguration_new();
  AConfiguration_fromAssetManager(config, g_App->activity->assetManager);
  int32_t density = AConfiguration_getDensity(config);
  AConfiguration_delete(config);
  
  const float kBaseDensity = 420.0f;
  const float kBaseScale = 3.0f;
  const float kGameScaleMultiplier = 1.8f;  // Extra scaling for in-game rendering only
  
  // Base DPI scale for font sizing
  float base_dpi_scale = density / kBaseDensity;
  
  // ImGui menu uses original base scale for element sizing (not DPI scaled)
  null::g_nullspace.scale = kBaseScale;
  
  // Game scale with multiplier for logical dimensions (in-game rendering only)
  float game_dpi_scale = base_dpi_scale * kGameScaleMultiplier;
  
  // Load Fonts with increased DPI scaling for better readability on high-DPI displays
  ImFontConfig font_cfg;
  font_cfg.SizePixels = 22.0f * base_dpi_scale * 1.5f;  // 1.5x larger font
  io.Fonts->AddFontDefault(&font_cfg);
  
  // Note: Ship texture will be loaded lazily after platform is set up
  
  // Calculate logical dimensions for game rendering (with extra multiplier for larger UI)
  // The game will render to these logical dimensions, and OpenGL will scale to physical resolution
  int logical_width = (int)(surface_width / game_dpi_scale);
  int logical_height = (int)(surface_height / game_dpi_scale);
  
  __android_log_print(ANDROID_LOG_INFO, g_LogTag, 
                      "Display density: %d, Base DPI: %.2f, Game DPI: %.2f, Physical: %dx%d, Logical: %dx%d", 
                      density, base_dpi_scale, game_dpi_scale, surface_width, surface_height, logical_width, logical_height);
  
  // Don't scale ImGui style - keep elements at original size
  // (Font is already scaled, windows use scale variable for sizing)

  g_Initialized = true;

  null::g_nullspace.Initialize();
  // Store logical dimensions for game rendering (DPI-scaled)
  null::g_nullspace.surface_width = logical_width;
  null::g_nullspace.surface_height = logical_height;
  // Store physical dimensions for framebuffer scaling
  null::g_nullspace.physical_width = surface_width;
  null::g_nullspace.physical_height = surface_height;

  glViewport(0, 0, surface_width, surface_height);
}

static void destroySurfaceOnly() {
  if (g_EglDisplay != EGL_NO_DISPLAY && g_EglSurface != EGL_NO_SURFACE) {
    __android_log_print(ANDROID_LOG_INFO, g_LogTag, "%s", "Destroying EGL surface only (keeping context alive)");
    eglMakeCurrent(g_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_EglDisplay, g_EglSurface);
    g_EglSurface = EGL_NO_SURFACE;
  }
}

static bool ensureSurface() {
  if (g_EglSurface != EGL_NO_SURFACE) return true;
  if (!g_EglContextCreated) return false;
  if (g_App == nullptr || g_App->window == nullptr) return false;

  __android_log_print(ANDROID_LOG_INFO, g_LogTag, "%s", "ensureSurface: recreating EGL surface");

  // Must reapply buffer geometry so the surface comes back at the same scaled size
  ANativeWindow_setBuffersGeometry(g_App->window, g_SurfaceBufferWidth, g_SurfaceBufferHeight, g_EglFormat);

  g_EglSurface = eglCreateWindowSurface(g_EglDisplay, g_EglConfig, g_App->window, nullptr);
  if (g_EglSurface == EGL_NO_SURFACE) {
    __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "ensureSurface: eglCreateWindowSurface failed: 0x%x", eglGetError());
    return false;
  }

  if (eglMakeCurrent(g_EglDisplay, g_EglSurface, g_EglSurface, g_EglContext) != EGL_TRUE) {
    __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "ensureSurface: eglMakeCurrent failed: 0x%x", eglGetError());
    eglDestroySurface(g_EglDisplay, g_EglSurface);
    g_EglSurface = EGL_NO_SURFACE;
    return false;
  }

  // Restore viewport to the physical dimensions
  glViewport(0, 0, g_PhysicalWidth, g_PhysicalHeight);

  __android_log_print(ANDROID_LOG_INFO, g_LogTag, "ensureSurface: EGL surface recreated, viewport %dx%d", g_PhysicalWidth, g_PhysicalHeight);
  return true;
}

void tick() {
  if (g_EglDisplay == EGL_NO_DISPLAY) return;

  // Lazily recreate EGL surface if lost (e.g. resume without INIT_WINDOW on Samsung)
  if (!ensureSurface()) return;

  if (!null::g_nullspace.Update()) {
    exit(0);
    return;
  }
}

void shutdown() {
  if (!g_Initialized) return;

  // Full shutdown — only called on actual app exit, not on background/resume
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplAndroid_Shutdown();
  ImGui::DestroyContext();

  if (g_EglDisplay != EGL_NO_DISPLAY) {
    eglMakeCurrent(g_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (g_EglContext != EGL_NO_CONTEXT) eglDestroyContext(g_EglDisplay, g_EglContext);

    if (g_EglSurface != EGL_NO_SURFACE) eglDestroySurface(g_EglDisplay, g_EglSurface);

    eglTerminate(g_EglDisplay);
  }

  g_EglDisplay = EGL_NO_DISPLAY;
  g_EglContext = EGL_NO_CONTEXT;
  g_EglSurface = EGL_NO_SURFACE;
  g_EglConfig = nullptr;
  g_EglFormat = 0;
  g_SurfaceBufferWidth = 0;
  g_SurfaceBufferHeight = 0;
  g_PhysicalWidth = 0;
  g_PhysicalHeight = 0;
  g_EglContextCreated = false;
  ANativeWindow_release(g_App->window);

  g_Initialized = false;

  if (null::g_nullspace.game) {
    null::g_nullspace.game->connection.SendDisconnect();
  }
}

static void handleAppCmd(struct android_app* app, int32_t appCmd) {
  switch (appCmd) {
    case APP_CMD_SAVE_STATE:
      break;
    case APP_CMD_INIT_WINDOW:
      // Full init on first launch; ensureSurface() handles subsequent resumes
      if (!g_Initialized) {
        init(app);
      } else {
        // Window came back — let ensureSurface() recreate lazily in tick()
        __android_log_print(ANDROID_LOG_INFO, g_LogTag, "%s", "APP_CMD_INIT_WINDOW: already initialized, will recreate surface lazily");
        ensureSurface();
      }
      break;
    case APP_CMD_TERM_WINDOW:
      // Only destroy the surface — keep EGLContext and all GL resources alive
      destroySurfaceOnly();
      break;
    case APP_CMD_GAINED_FOCUS:
      // Surface may have returned without INIT_WINDOW on Samsung — try lazily
      ensureSurface();
      break;
    case APP_CMD_LOST_FOCUS:
      break;
  }
}

static int32_t handleInputEvent(struct android_app* app, AInputEvent* inputEvent) {
  if (AInputEvent_getType(inputEvent) == AINPUT_EVENT_TYPE_MOTION) {
    int screen_width = ANativeWindow_getWidth(app->window);
    int screen_height = ANativeWindow_getHeight(app->window);

    int action = AMotionEvent_getAction(inputEvent);
    unsigned int flags = action & AMOTION_EVENT_ACTION_MASK;
    size_t pointer_index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pointer_count = AMotionEvent_getPointerCount(inputEvent);

    // Check if onboarding wizard is active and handle tap
    null::Game* game = null::g_nullspace.game;
    if (game && game->onboarding.IsActive() && flags == AMOTION_EVENT_ACTION_DOWN) {
      game->onboarding.OnTap();
      return 1;  // Consume the event
    }

    // Use first pointer for primary touch (flying/menu/spectate)
    float x = AMotionEvent_getX(inputEvent, 0);
    float y = AMotionEvent_getY(inputEvent, 0);
    
    float nx = (x / screen_width) - 0.5f;
    float ny = (y / screen_height) - 0.5f;

    if (game) {
      null::Player* self = game->player_manager.GetSelf();

      if (self) {
        // Check ALL active pointers for weapon button presses (multi-touch support)
        if (self->ship < 8 && (flags == AMOTION_EVENT_ACTION_MOVE || flags == AMOTION_EVENT_ACTION_POINTER_DOWN || flags == AMOTION_EVENT_ACTION_DOWN)) {
          bool gun_pressed = false;
          bool bomb_pressed = false;
          
          for (size_t i = 0; i < pointer_count; ++i) {
            float px = AMotionEvent_getX(inputEvent, i);
            float py = AMotionEvent_getY(inputEvent, i);
            float logical_x = px * (null::g_nullspace.surface_width / (float)screen_width);
            float logical_y = py * (null::g_nullspace.surface_height / (float)screen_height);
            float logical_screen_width = (float)null::g_nullspace.surface_width;
            float logical_screen_height = (float)null::g_nullspace.surface_height;
            
            // Weapon button dimensions (round buttons)
            float button_size = 72.0f;
            float button_radius = button_size / 2.0f;
            float button_spacing = 12.0f;
            float button_y = logical_screen_height - button_size - 10.0f;
            float button_center_y = button_y + button_radius;
            float gun_button_x = logical_screen_width - (button_size * 2) - button_spacing - 40.0f;  // Adjusted margin
            float bomb_button_x = gun_button_x + button_size + button_spacing;
            float gun_cx = gun_button_x + button_radius;
            float bomb_cx = bomb_button_x + button_radius;

            // Circular hit test for gun button
            float gdx = logical_x - gun_cx;
            float gdy = logical_y - button_center_y;
            float bdx = logical_x - bomb_cx;
            float bdy = logical_y - button_center_y;
            if (gdx * gdx + gdy * gdy <= button_radius * button_radius * 1.5f) {
              gun_pressed = true;
            } else if (bdx * bdx + bdy * bdy <= button_radius * button_radius * 1.5f) {
              bomb_pressed = true;
            }
            
            // Check if this pointer is on ability icon (ONLY on DOWN events, not MOVE)
            if ((flags == AMOTION_EVENT_ACTION_DOWN || flags == AMOTION_EVENT_ACTION_POINTER_DOWN) && !android_input.abilities_triggered) {
              float item_stack_height = 175.0f;
              float items_start_y = logical_screen_height - item_stack_height - 10.0f;
              float item_width = 26.0f;
              float item_height = 25.0f;
              
              if (logical_x >= 0 && logical_x <= item_width && logical_y >= items_start_y) {
                int item_index = (int)((logical_y - items_start_y) / item_height);
                if (item_index >= 0 && item_index < 7) {
                  // Trigger ability once on DOWN
                  null::InputAction action;
                  switch (item_index) {
                    case 0: action = null::InputAction::Burst; break;
                    case 1: action = null::InputAction::Repel; break;
                    case 2: action = null::InputAction::Decoy; break;
                    case 3: action = null::InputAction::Thor; break;
                    case 4: action = null::InputAction::Brick; break;
                    case 5: action = null::InputAction::Rocket; break;
                    case 6: {
                      // Portal/Warp toggle - if portal is active, warp to it, otherwise drop portal
                      if (game->ship_controller.ship.portal_time > 0.0f) {
                        action = null::InputAction::Warp;
                      } else {
                        action = null::InputAction::Portal;
                      }
                    } break;
                    default: action = null::InputAction::Burst; break;
                  }
                  null::g_InputState.SetAction(action, true);
                  android_input.abilities_triggered = true;
                }
              }
              
              // Check portal timer touch (for warping when portal is active)
              if (game->ship_controller.ship.portal_time > 0.0f) {
                // Portal timer is right-anchored at y = screen_height - 170 - 19
                float portal_timer_y = logical_screen_height - 189.0f;
                float portal_timer_height = 16.0f;
                float portal_timer_width = 50.0f;  // Icon + text width estimate
                float portal_timer_x = logical_screen_width - portal_timer_width;
                
                if (logical_x >= portal_timer_x && logical_x <= logical_screen_width &&
                    logical_y >= portal_timer_y && logical_y <= (portal_timer_y + portal_timer_height)) {
                  // Tap on portal timer - warp to portal
                  null::g_InputState.SetAction(null::InputAction::Warp, true);
                  android_input.abilities_triggered = true;
                }
              }
              
              // Check right-side togglable icons (gun, bomb, stealth, cloak, xradar, antiwarp)
              float right_icon_x = logical_screen_width - 26.0f;
              float right_start_y = logical_screen_height - 160.0f;
              float right_icon_width = 26.0f;
              float right_icon_height = 25.0f;
              
              if (logical_x >= right_icon_x && logical_x <= logical_screen_width && logical_y >= right_start_y) {
                int right_index = (int)((logical_y - right_start_y) / right_icon_height);
                // Index 0 = gun (multifire toggle), 1 = bomb (mine mode toggle), 2 = stealth, 3 = cloak, 4 = xradar, 5 = antiwarp
                if (right_index == 0) {
                  // Multifire toggle on gun icon
                  if (null::g_InputState.action_callback) {
                    null::g_InputState.action_callback(null::g_InputState.user, null::InputAction::Multifire);
                  }
                  android_input.abilities_triggered = true;
                } else if (right_index == 1) {
                  // Mine mode toggle on bomb icon
                  android_input.mine_mode_active = !android_input.mine_mode_active;
                  null::g_AndroidMineMode = android_input.mine_mode_active;
                  // Play sound feedback
                  if (android_input.mine_mode_active) {
                    game->sound_system.Play(null::AudioType::Mine1);
                  } else {
                    game->sound_system.Play(null::AudioType::ToggleOff);
                  }
                  android_input.abilities_triggered = true;
                } else if (right_index >= 2 && right_index <= 5) {
                  null::InputAction action;
                  switch (right_index) {
                    case 2: action = null::InputAction::Stealth; break;
                    case 3: action = null::InputAction::Cloak; break;
                    case 4: action = null::InputAction::XRadar; break;
                    case 5: action = null::InputAction::Antiwarp; break;
                    default: action = null::InputAction::Stealth; break;
                  }
                  // Use action callback for togglables instead of SetAction
                  if (null::g_InputState.action_callback) {
                    null::g_InputState.action_callback(null::g_InputState.user, action);
                  }
                  android_input.abilities_triggered = true;
                }
              }
            }
          }
          
          // Set weapon actions based on what's pressed and mine mode toggle
          null::g_InputState.SetAction(null::InputAction::Bullet, gun_pressed);
          
          // Bomb button respects mine mode toggle
          if (bomb_pressed) {
            null::g_InputState.SetAction(null::InputAction::Bomb, !android_input.mine_mode_active);
            null::g_InputState.SetAction(null::InputAction::Mine, android_input.mine_mode_active);
          } else {
            null::g_InputState.SetAction(null::InputAction::Bomb, false);
            null::g_InputState.SetAction(null::InputAction::Mine, false);
          }
        }
        
        if (flags == AMOTION_EVENT_ACTION_DOWN) {
          android_input.touch_start_time = AMotionEvent_getEventTime(inputEvent);
          android_input.touch_start_x = x;
          android_input.touch_start_y = y;
          android_input.long_press_triggered = false;
          // Weapon button handling moved to multi-touch section above
        } else if (flags == AMOTION_EVENT_ACTION_UP) {
          android_input.anchored = false;
          android_input.joystick_active = false;
          android_input.joystick_x = 0;
          android_input.joystick_y = 0;
          android_input.abilities_triggered = false;  // Reset ability trigger flag
          
          // Clear ship control inputs when touch is released
          if (self->ship < 8) {
            null::g_InputState.SetAction(null::InputAction::Forward, false);
            null::g_InputState.SetAction(null::InputAction::Left, false);
            null::g_InputState.SetAction(null::InputAction::Right, false);
            null::g_InputState.SetAction(null::InputAction::Afterburner, false);
            null::g_InputState.SetAction(null::InputAction::Bullet, false);
            null::g_InputState.SetAction(null::InputAction::Bomb, false);
            null::g_InputState.SetAction(null::InputAction::Mine, false);
            // Clear ability actions (they were pulsed on tap)
            null::g_InputState.SetAction(null::InputAction::Burst, false);
            null::g_InputState.SetAction(null::InputAction::Repel, false);
            null::g_InputState.SetAction(null::InputAction::Decoy, false);
            null::g_InputState.SetAction(null::InputAction::Thor, false);
            null::g_InputState.SetAction(null::InputAction::Brick, false);
            null::g_InputState.SetAction(null::InputAction::Rocket, false);
            null::g_InputState.SetAction(null::InputAction::Portal, false);
          }
          
          int64_t touch_duration = AMotionEvent_getEventTime(inputEvent) - android_input.touch_start_time;
          
          // Calculate drag distance to distinguish tap from drag
          float dx_tap = x - android_input.touch_start_x;
          float dy_tap = y - android_input.touch_start_y;
          bool is_tap = (dx_tap * dx_tap + dy_tap * dy_tap) < 400.0f;  // 20px threshold
          
          // Transform touch coordinates from physical to logical space for menu detection
          float logical_x = x * (null::g_nullspace.surface_width / (float)screen_width);
          float logical_y = y * (null::g_nullspace.surface_height / (float)screen_height);
          
          bool handled_menu_interaction = false;
          
          // If showing all statboxes, any tap closes them
          if (game->show_all_statboxes && is_tap) {
            game->show_all_statboxes = false;
            handled_menu_interaction = true;
          }
          // If menu is open, handle button clicks first (use logical coordinates)
          else if (game->menu_open && is_tap) {
            handled_menu_interaction = true;
            
            // Menu dimensions: 620x340, top at y=3 (in logical space)
            float menu_width = 620.0f;
            float menu_height = 340.0f;
            float logical_screen_width = (float)null::g_nullspace.surface_width;
            float menu_x = (logical_screen_width - menu_width) * 0.5f;
            float menu_y = 3.0f;
            
            // Left column (Quit, Scoreboard, Help)
            float left_button_width = 140.0f;
            float left_button_height = 35.0f;
            float left_column_x = menu_x + 10.0f;
            float left_start_y = menu_y + 20.0f;
            
            // Right side ship grid
            float grid_start_x = menu_x + left_button_width + 40.0f;
            float grid_start_y = menu_y + 40.0f;
            float cell_width = 110.0f;
            float cell_height = 70.0f;
            float grid_spacing = 8.0f;
            
            bool handled_click = false;
            
            // Check left column buttons
            if (logical_x >= left_column_x && logical_x <= left_column_x + left_button_width &&
                logical_y >= left_start_y && logical_y <= left_start_y + 3 * (left_button_height + 8.0f)) {
              float relative_y = logical_y - left_start_y;
              int button_index = (int)(relative_y / (left_button_height + 8.0f));
              
              if (button_index == 0) {
                // Quit
                null::g_InputState.OnCharacter('q');
                handled_click = true;
              } else if (button_index == 1) {
                // Scoreboard - toggle multi-statbox display
                game->show_all_statboxes = !game->show_all_statboxes;
                game->menu_open = false;
                handled_click = true;
              } else if (button_index == 2) {
                // Help - show onboarding wizard
                game->onboarding.Show();
                game->menu_open = false;
                handled_click = true;
              }
            }
            
            // Check ship grid (3x3 grid, 9 cells for 8 ships + spectator)
            if (!handled_click && logical_x >= grid_start_x && logical_x <= grid_start_x + 3 * (cell_width + grid_spacing) &&
                logical_y >= grid_start_y && logical_y <= grid_start_y + 3 * (cell_height + grid_spacing)) {
              float relative_x = logical_x - grid_start_x;
              float relative_y = logical_y - grid_start_y;
              
              int col = (int)(relative_x / (cell_width + grid_spacing));
              int row = (int)(relative_y / (cell_height + grid_spacing));
              
              if (col >= 0 && col < 3 && row >= 0 && row < 3) {
                int ship_index = row * 3 + col;
                
                if (ship_index >= 0 && ship_index <= 8) {
                  if (ship_index == 8) {
                    null::g_InputState.OnCharacter('s');  // Spectator
                  } else {
                    // Ships 0-7 map to keys '1'-'8'
                    null::g_InputState.OnCharacter('1' + ship_index);
                  }
                  handled_click = true;
                }
              }
            }
            
            // If click is anywhere else in menu but not handled, keep menu open
            // If click outside menu, close it
            if (!handled_click) {
              if (logical_x >= menu_x && logical_x <= menu_x + menu_width &&
                  logical_y >= menu_y && logical_y <= menu_y + menu_height) {
                // Click inside menu but not on a working button - do nothing (keep menu open)
              } else {
                // Click outside menu - close it
                null::g_InputState.OnCharacter(NULLSPACE_KEY_ESCAPE);
              }
            }
          } else {
            android_input.last_tap_time = AMotionEvent_getEventTime(inputEvent);
            android_input.last_tap_x = x;
            android_input.last_tap_y = y;
            
            // Check if tap is in top UI area (health bar, name, etc) - roughly first 80 pixels (in logical space)
            bool is_top_ui = (logical_y < 80.0f) && is_tap;
            
            if (is_top_ui && touch_duration < LONG_PRESS_TIMEOUT) {
              // Toggle menu when tapping top UI area
              null::g_InputState.OnCharacter(NULLSPACE_KEY_ESCAPE);
              handled_menu_interaction = true;
            }
            // Ability buttons are handled via multi-touch above
            // Tap-to-shoot disabled for now - was causing bullet to get stuck
            // Can re-enable later with better implementation
            /*
            else if (self->ship < 8 && is_tap && touch_duration < LONG_PRESS_TIMEOUT && !handled_menu_interaction) {
              // Quick tap while in ship = shoot guns (but not if we just did menu stuff)
              null::g_InputState.SetAction(null::InputAction::Bullet, true);
            }
            */
          }
        } else {
          // Handle drag/move input
          if (self->ship == 8) {
            // Spectator mode: drag to move camera
            if (!android_input.anchored) {
              android_input.world_x = self->position.x;
              android_input.world_y = self->position.y;
              android_input.nx = nx;
              android_input.ny = ny;
              android_input.anchored = true;
            }

            float offset_x = (android_input.nx - nx) * (game->ui_camera.surface_dim.x * (1.0f / 16.0f));
            float offset_y = (android_input.ny - ny) * (game->ui_camera.surface_dim.y * (1.0f / 16.0f));

            self->position.x = android_input.world_x + offset_x;
            self->position.y = android_input.world_y + offset_y;

            if (abs(offset_x) > 1.0f || abs(offset_y) > 1.0f) {
              game->specview.spectate_id = null::kInvalidSpectateId;
            }

            int64_t eventTime = AMotionEvent_getEventTime(inputEvent);
            if (eventTime - android_input.last_tap_time <= DOUBLE_TAP_TIMEOUT) {
              float dx = x - android_input.last_tap_x;
              float dy = y - android_input.last_tap_y;

              if (dx * dx + dy * dy < DOUBLE_TAP_SLOP * DOUBLE_TAP_SLOP) {
                null::Vector2f world_pos(self->position.x + nx * (game->ui_camera.surface_dim.x * (1.0f / 16.0f)),
                                         self->position.y + ny * (game->ui_camera.surface_dim.y * (1.0f / 16.0f)));

                // Find nearby player to spectate
                null::Player* closest = nullptr;
                float closest_dist = 10000.0f;

                for (size_t i = 0; i < game->player_manager.player_count; ++i) {
                  null::Player* player = game->player_manager.players + i;

                  if (player->ship == 8) continue;

                  float dist_sq = player->position.DistanceSq(world_pos);
                  if (dist_sq < closest_dist) {
                    closest_dist = dist_sq;
                    closest = player;
                  }
                }

                if (closest && closest_dist <= 4.0f * 4.0f) {
                  game->specview.spectate_id = closest->id;
                  game->specview.spectate_frequency = closest->frequency;
                }
              }
            }
          } else {
            // Ship mode: Virtual D-Pad in bottom-left for flying
            float logical_x = x * (null::g_nullspace.surface_width / (float)screen_width);
            float logical_y = y * (null::g_nullspace.surface_height / (float)screen_height);
            float logical_screen_height = (float)null::g_nullspace.surface_height;
            
            // D-pad position: same as weapon buttons but on left side
            float dpad_size = 140.0f;  // Increased from 80
            float dpad_radius = dpad_size / 2.0f;
            float dpad_center_x = 50.0f + dpad_radius;  // Adjusted: 50px from left + radius
            float button_size = 72.0f;
            float button_y = logical_screen_height - button_size - 10.0f;
            float dpad_center_y = button_y + button_size / 2.0f - 40.0f - (logical_screen_height * 0.05f);  // 40px + 5% screen height higher than buttons
            
            // Check if touch is in d-pad area (generous extended zone for dragging outside)
            float dx = logical_x - dpad_center_x;
            float dy = logical_y - dpad_center_y;
            float dist = sqrtf(dx * dx + dy * dy);
            
            // Allow dragging well outside the d-pad - up to 3x radius
            if (dist < dpad_radius * 3.0f) {
              android_input.joystick_active = (dist > 15.0f);  // Dead zone (increased from 10)
              
              if (android_input.joystick_active) {
                // Calculate desired angle from touch direction relative to d-pad center
                // Fix: Add 90 degrees to correct the rotation
                float touch_angle = atan2f(dy, dx) + (3.14159265f / 2.0f);  // Add 90 degrees
                float desired_orientation = touch_angle / (2.0f * 3.14159265f);  // 0-1 range
                if (desired_orientation < 0) desired_orientation += 1.0f;
                
                // Calculate angle difference to current ship orientation
                float angle_diff = desired_orientation - self->orientation;
                // Normalize to shortest rotation path
                if (angle_diff > 0.5f) angle_diff -= 1.0f;
                if (angle_diff < -0.5f) angle_diff += 1.0f;
                
                // Set rotation to turn ship toward desired direction
                const float kRotationThreshold = 0.02f;  // ~7 degrees (tighter than before)
                if (angle_diff < -kRotationThreshold) {
                  null::g_InputState.SetAction(null::InputAction::Left, true);
                  null::g_InputState.SetAction(null::InputAction::Right, false);
                } else if (angle_diff > kRotationThreshold) {
                  null::g_InputState.SetAction(null::InputAction::Right, true);
                  null::g_InputState.SetAction(null::InputAction::Left, false);
                } else {
                  // Close enough to desired direction - stop rotating
                  null::g_InputState.SetAction(null::InputAction::Left, false);
                  null::g_InputState.SetAction(null::InputAction::Right, false);
                }
                
                // Afterburner: activate when dragging beyond d-pad border
                bool afterburner_active = (dist > dpad_radius);
                null::g_InputState.SetAction(null::InputAction::Afterburner, afterburner_active);
                
                // Always thrust forward when d-pad is pushed (even while rotating)
                null::g_InputState.SetAction(null::InputAction::Forward, true);
              } else {
                // In dead zone - clear all
                null::g_InputState.SetAction(null::InputAction::Forward, false);
                null::g_InputState.SetAction(null::InputAction::Left, false);
                null::g_InputState.SetAction(null::InputAction::Right, false);
                null::g_InputState.SetAction(null::InputAction::Afterburner, false);
              }
            } else {
              // Not in d-pad area - clear joystick and movement
              android_input.joystick_active = false;
              null::g_InputState.SetAction(null::InputAction::Forward, false);
              null::g_InputState.SetAction(null::InputAction::Left, false);
              null::g_InputState.SetAction(null::InputAction::Right, false);
              null::g_InputState.SetAction(null::InputAction::Afterburner, false);
            }
          }
        }
      }
    }
  }
  return ImGui_ImplAndroid_HandleInputEvent(inputEvent);
}

void android_main(struct android_app* app) {
  app->onAppCmd = handleAppCmd;
  app->onInputEvent = handleInputEvent;

  while (true) {
    int out_events;
    struct android_poll_source* out_data;

    // Poll all events. If the app is not visible, this loop blocks until g_Initialized == true.
    while (ALooper_pollAll(g_Initialized ? 0 : -1, NULL, &out_events, (void**)&out_data) >= 0) {
      // Process one event
      if (out_data != NULL) out_data->process(app, out_data);

      // Exit the app by returning from within the infinite loop
      if (app->destroyRequested != 0) {
        // shutdown() should have been called already while processing the
        // app command APP_CMD_TERM_WINDOW. But we play save here
        if (!g_Initialized) shutdown();

        return;
      }
    }

    // Initiate a new frame
    tick();
  }
}

// Unfortunately, there is no way to show the on-screen input from native code.
// Therefore, we call ShowSoftKeyboardInput() of the main activity implemented in MainActivity.kt via JNI.
static int ShowSoftKeyboardInput() {
  JavaVM* java_vm = g_App->activity->vm;
  JNIEnv* java_env = NULL;

  jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
  if (jni_return == JNI_ERR) return -1;

  jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
  if (jni_return != JNI_OK) return -2;

  jclass native_activity_clazz = java_env->GetObjectClass(g_App->activity->clazz);
  if (native_activity_clazz == NULL) return -3;

  jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "showSoftInput", "()V");
  if (method_id == NULL) return -4;

  java_env->CallVoidMethod(g_App->activity->clazz, method_id);

  jni_return = java_vm->DetachCurrentThread();
  if (jni_return != JNI_OK) return -5;

  return 0;
}

static const char* AndroidGetStoragePath(null::MemoryArena& arena, const char* path) {
  JavaVM* java_vm = g_App->activity->vm;
  JNIEnv* env = NULL;

  jint jni_return = java_vm->GetEnv((void**)&env, JNI_VERSION_1_6);
  if (jni_return == JNI_ERR) return nullptr;

  jni_return = java_vm->AttachCurrentThread(&env, NULL);
  if (jni_return != JNI_OK) return nullptr;

  jclass cls_Env = env->FindClass("android/app/NativeActivity");
  jmethodID mid = env->GetMethodID(cls_Env, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
  jobject obj_File = env->CallObjectMethod(g_App->activity->clazz, mid, NULL);
  jclass cls_File = env->FindClass("java/io/File");
  jmethodID mid_getPath = env->GetMethodID(cls_File, "getPath", "()Ljava/lang/String;");
  jstring external_path = (jstring)env->CallObjectMethod(obj_File, mid_getPath);

  const char* c_path = env->GetStringUTFChars(external_path, NULL);

  char* result = (char*)arena.Allocate(strlen(c_path) + strlen(path) + 1);

  sprintf(result, "%s/%s", c_path, path);

  env->ReleaseStringUTFChars(external_path, c_path);
  java_vm->DetachCurrentThread();

  return result;
}

// Unfortunately, the native KeyEvent implementation has no getUnicodeChar() function.
// Therefore, we implement the processing of KeyEvents in MainActivity.kt and poll
// the resulting Unicode characters here via JNI and send them to Dear ImGui.
static int PollUnicodeChars() {
  JavaVM* java_vm = g_App->activity->vm;
  JNIEnv* java_env = NULL;

  jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
  if (jni_return == JNI_ERR) return -1;

  jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
  if (jni_return != JNI_OK) return -2;

  jclass native_activity_clazz = java_env->GetObjectClass(g_App->activity->clazz);
  if (native_activity_clazz == NULL) return -3;

  jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "pollUnicodeChar", "()I");
  if (method_id == NULL) return -4;

  // Send the actual characters to Dear ImGui
  ImGuiIO& io = ImGui::GetIO();
  jint unicode_character;
  while ((unicode_character = java_env->CallIntMethod(g_App->activity->clazz, method_id)) != 0)
    io.AddInputCharacter(unicode_character);

  jni_return = java_vm->DetachCurrentThread();
  if (jni_return != JNI_OK) return -5;

  return 0;
}

static unsigned char* AndroidLoadAsset(const char* filename, size_t* size) {
  unsigned char* data = nullptr;
  *size = 0;

  AAsset* asset_descriptor = AAssetManager_open(g_App->activity->assetManager, filename, AASSET_MODE_BUFFER);
  if (asset_descriptor) {
    *size = AAsset_getLength(asset_descriptor);
    data = (unsigned char*)malloc(*size);
    int64_t num_bytes_read = AAsset_read(asset_descriptor, data, *size);
    AAsset_close(asset_descriptor);
    IM_ASSERT(num_bytes_read == *size);
  }

  return data;
}

static unsigned char* AndroidLoadAssetArena(null::MemoryArena& arena, const char* filename, size_t* size) {
  unsigned char* data = nullptr;
  *size = 0;

  AAsset* asset_descriptor = AAssetManager_open(g_App->activity->assetManager, filename, AASSET_MODE_BUFFER);
  if (asset_descriptor) {
    *size = AAsset_getLength(asset_descriptor);
    data = arena.Allocate(*size);
    int64_t num_bytes_read = AAsset_read(asset_descriptor, data, *size);
    AAsset_close(asset_descriptor);
    IM_ASSERT(num_bytes_read == *size);
  }

  return data;
}

static void AndroidLogger(const char* fmt, ...) {
  va_list args;

  va_start(args, fmt);
  __android_log_vprint(ANDROID_LOG_ERROR, g_LogTag, fmt, args);
  va_end(args);
}

static unsigned int AndroidGetMachineId() {
  return rand();
}

static int AndroidGetTimeZoneBias() {
  return 240;
}

static void SetPlatform() {
  static const null::Platform kAndroidPlatform = {AndroidLogger,
                                                  AndroidLogger,
                                                  AndroidGetStoragePath,
                                                  AndroidLoadAsset,
                                                  AndroidLoadAssetArena,
                                                  null::platform.CreateFolder,
                                                  null::platform.PasteClipboard,
                                                  AndroidGetMachineId,
                                                  AndroidGetTimeZoneBias};

  null::platform = kAndroidPlatform;
}
