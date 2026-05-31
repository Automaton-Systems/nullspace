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
#include <null/android/AndroidSettings.h>

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
bool g_MobileMineMode = false;

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
const char* kArenaName = "";

struct ServerInfo {
  const char* name;
  const char* server;
  u16 port;
};

ServerInfo kServers[] = {
    {"emulator", "10.0.2.2", 5000},
    {"Null Orbit", "api.null-orbit.com", 5000},
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
  std::chrono::steady_clock::time_point last_frame_time = {};
  float scale = 1.0f;

  size_t selected_zone_index = 0;

  std::string GenerateRandomUsername() {
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
    
    const char* word1 = first[rand() % (sizeof(first) / sizeof(first[0]))];
    const char* word2 = second[rand() % (sizeof(second) / sizeof(second[0]))];
    int random_number = rand() % 10000;
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s%s%04d", word1, word2, random_number);
    return std::string(buffer);
  }

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

    srand(static_cast<unsigned>(time(nullptr)));
    
    // Load username from settings or generate new one
    if (!g_AndroidSettings.username.empty()) {
      snprintf(name, sizeof(name), "%s", g_AndroidSettings.username.c_str());
    } else {
      std::string generated = GenerateRandomUsername();
      snprintf(name, sizeof(name), "%s", generated.c_str());
      g_AndroidSettings.SetUsername(generated);
    }
    strcpy(password, kPlayerPassword);

    SetPlatform();

    return true;
  }

  bool JoinZone(size_t selected_index, const char* arena_name) {
    kServerName = kServers[selected_index].name;
    kServerIp = kServers[selected_index].server;
    kServerPort = kServers[selected_index].port;
    kArenaName = arena_name;

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

    // iOS-style: Pure black background, no sprites
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_Border] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ImGui::GetStyle().Colors[ImGuiCol_Button] = ImVec4(0.145f, 0.145f, 0.145f, 1.0f);  // Dark gray #252525
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = ImVec4(0.227f, 0.227f, 0.227f, 1.0f);  // #3A3A3A
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.937f, 0.678f, 0.129f, 1.0f);  // Gold #EFAD21
    ImGui::GetStyle().WindowBorderSize = 0.0f;
    ImGui::GetStyle().WindowPadding = ImVec2(0.0f, 0.0f);

    // Full screen window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoScrollbar;
    
    if (ImGui::Begin("LoginScreen", nullptr, window_flags)) {
      float center_x = io.DisplaySize.x * 0.5f;
      
      // Get/generate username
      if (!g_AndroidSettings.username.empty()) {
        snprintf(name, sizeof(name), "%s", g_AndroidSettings.username.c_str());
      } else {
        // First launch - generate random username
        std::string new_name = GenerateRandomUsername();
        snprintf(name, sizeof(name), "%s", new_name.c_str());
        g_AndroidSettings.SetUsername(new_name);
      }
      
      // ═══════════════════════════════════════════════════════════════
      // iOS-STYLE CLEAN MENU (matching ViewController.mm layout)
      // ═══════════════════════════════════════════════════════════════
      
      // TITLE: "NULLORBIT" at 8% down (matching iOS)
      float title_y = io.DisplaySize.y * 0.08f;
      ImGui::SetCursorPosY(title_y);
      ImGui::SetWindowFontScale(5.0f);
      float title_width = ImGui::CalcTextSize("NULLORBIT").x;
      ImGui::SetCursorPosX(center_x - title_width * 0.5f);
      ImGui::TextColored(ImVec4(0.710f, 0.710f, 1.0f, 1.0f), "NULLORBIT");  // Light blue #B5B5FF
      ImGui::SetWindowFontScale(1.0f);
      
      // USERNAME SECTION: at 28% down (matching iOS)
      float username_y = io.DisplaySize.y * 0.28f;
      ImGui::SetCursorPosY(username_y);
      
      // Player label and username centered
      ImGui::SetWindowFontScale(1.7f);
      float player_text_width = ImGui::CalcTextSize("Player: ").x;
      float name_width = ImGui::CalcTextSize(name).x;
      float total_width = player_text_width + name_width;
      ImGui::SetCursorPosX(center_x - total_width * 0.5f);
      
      ImGui::TextColored(ImVec4(0.451f, 1.0f, 0.388f, 1.0f), "Player: %s", name);  // Neon green
      ImGui::SetWindowFontScale(1.0f);
      
      // RESET button on far right (wider to fit "RESET NAME" text)
      float reset_width = 380.0f;
      float reset_height = 100.0f;
      ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - reset_width - 20.0f, username_y));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.871f, 0.192f, 0.031f, 1.0f));  // Red #DE3108
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
      ImGui::SetWindowFontScale(1.5f);  // Larger font for bigger button
      if (ImGui::Button("RESET NAME", ImVec2(reset_width, reset_height))) {
        std::string new_name = GenerateRandomUsername();
        snprintf(name, sizeof(name), "%s", new_name.c_str());
        g_AndroidSettings.SetUsername(new_name);
      }
      ImGui::SetWindowFontScale(1.0f);  // Reset font scale
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(2);
      
      // ARENA SELECTION LABEL: at 42% down (above buttons, with margin from username)
      float label_y = io.DisplaySize.y * 0.42f;
      ImGui::SetCursorPosY(label_y);
      ImGui::SetWindowFontScale(1.6f);
      float label_width = ImGui::CalcTextSize("Select an arena").x;
      ImGui::SetCursorPosX(center_x - label_width * 0.5f);
      ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Select an arena");
      ImGui::SetWindowFontScale(1.0f);
      
      // GAME MODE BUTTONS: at 50% down (with margin from label)
      float buttons_y = io.DisplaySize.y * 0.50f;
      float button_margin = io.DisplaySize.x * 0.10f;  // 10% margins on each side
      float button_width = io.DisplaySize.x - (button_margin * 2);
      float tw_button_height = io.DisplaySize.y * 0.15f;  // 15% of screen height (taller)
      float button_spacing = io.DisplaySize.y * 0.04f;  // 4% of screen height for spacing
      
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
      
      // Trench Wars: Capture the Flag button (taller)
      ImGui::SetCursorPos(ImVec2(button_margin, buttons_y));
      ImGui::SetWindowFontScale(1.8f);
      if (ImGui::Button("Trench Wars: Capture the Flag", ImVec2(button_width, tw_button_height))) {
        selected_zone_index = 1;  // Null Orbit production server
        JoinZone(selected_zone_index, "pub");
      }
      ImGui::SetWindowFontScale(1.0f);
      
      // TDM buttons side by side (below Trench Wars)
      float tdm_buttons_y = buttons_y + tw_button_height + button_spacing;
      float button_padding = io.DisplaySize.x * 0.04f;  // 4% of screen width between buttons
      float tdm_button_width = (button_width - button_padding) / 2.0f;
      float tdm_button_height = io.DisplaySize.y * 0.12f;  // 12% of screen height (shorter than TW)
      
      // Team Deathmatch button (left)
      ImGui::SetCursorPos(ImVec2(button_margin, tdm_buttons_y));
      ImGui::SetWindowFontScale(1.8f);
      if (ImGui::Button("Team Deathmatch", ImVec2(tdm_button_width, tdm_button_height))) {
        selected_zone_index = 1;  // Null Orbit production server
        JoinZone(selected_zone_index, "tdm");
      }
      ImGui::SetWindowFontScale(1.0f);
      
      // Team Deathmatch Chaos button (right) - using spaces to help center "Chaos!"
      ImGui::SetCursorPos(ImVec2(button_margin + tdm_button_width + button_padding, tdm_buttons_y));
      ImGui::SetWindowFontScale(1.8f);
      if (ImGui::Button("Team Deathmatch\n     Chaos!", ImVec2(tdm_button_width, tdm_button_height))) {
        selected_zone_index = 1;  // Null Orbit production server
        JoinZone(selected_zone_index, "chaos");
      }
      ImGui::SetWindowFontScale(1.0f);
      
      ImGui::PopStyleVar();  // FrameRounding
      
      // EMULATOR BUTTON (debug only) - small button in top-right corner
      #ifndef NDEBUG
        float emu_size = 100.0f;
        float emu_margin = 20.0f;
        ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - emu_size - emu_margin, emu_margin));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        ImGui::SetWindowFontScale(1.0f);
        if (ImGui::Button("EMU", ImVec2(emu_size, emu_size))) {
          selected_zone_index = 0;  // Emulator server
          JoinZone(selected_zone_index, "");
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleVar();
      #endif
      
      ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return true;
  }

  bool Update() {
    constexpr float kMaxDelta = 1.0f / 20.0f;

    auto current_frame_time = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;

    if (last_frame_time != std::chrono::steady_clock::time_point{}) {
      dt = std::chrono::duration<float>(current_frame_time - last_frame_time).count();
    }

    last_frame_time = current_frame_time;

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

  // Calculate scale based on display density and screen size for better DPI awareness
  // Detect tablet vs phone to apply appropriate scaling
  AConfiguration* config = AConfiguration_new();
  AConfiguration_fromAssetManager(config, g_App->activity->assetManager);
  int32_t density = AConfiguration_getDensity(config);
  int32_t screen_width_dp = AConfiguration_getScreenWidthDp(config);
  int32_t screen_height_dp = AConfiguration_getScreenHeightDp(config);
  AConfiguration_delete(config);
  
  // Determine smallest width in dp (for tablet detection)
  int32_t smallest_width_dp = (screen_width_dp < screen_height_dp) ? screen_width_dp : screen_height_dp;
  bool is_tablet = smallest_width_dp >= 600;  // 600dp is Android's tablet threshold
  
  // Reference: Galaxy S25U is 560 DPI, works well with (560/420)*1.8 = 2.4 scale
  // Reference: Typical tablet is 240-320 DPI, needs different approach
  // Reference: iPhone 8+ equiv ~326 DPI would get (326/420)*1.8 = 1.4 scale (too small, overflows)
  
  const float kBaseDensity = 420.0f;
  const float kPhoneScaleMultiplier = 1.8f;
  const float kTabletScaleMultiplier = 3.0f;  // Tablets use higher multiplier to maintain readable UI
  
  // ImGui menu uses base scale for element sizing
  const float kBaseScale = 3.0f;
  null::g_nullspace.scale = kBaseScale;
  
  // Calculate density scale
  float density_scale = density / kBaseDensity;
  
  // Apply different multipliers for phones vs tablets
  float game_scale;
  if (is_tablet) {
    // Tablets: Higher multiplier to keep UI readable on larger screens
    // Matches iOS tablet approach (~2x scale for typical tablet DPI)
    game_scale = density_scale * kTabletScaleMultiplier;
  } else {
    // Phones: Original scaling formula, but with clamping for low-DPI devices
    game_scale = density_scale * kPhoneScaleMultiplier;
    
    // Clamp to prevent overflow on low-resolution screens
    // Minimum 1.4 keeps UI readable even on low-DPI devices
    if (game_scale < 1.4f) game_scale = 1.4f;
  }
  
  // Calculate density scale for font sizing
  float base_dpi_scale = density / 420.0f;
  
  // Load Fonts with DPI scaling for readability
  ImFontConfig font_cfg;
  font_cfg.SizePixels = 22.0f * base_dpi_scale * 1.5f;
  io.Fonts->AddFontDefault(&font_cfg);
  
  // Calculate logical dimensions for game rendering
  int logical_width = (int)(surface_width / game_scale);
  int logical_height = (int)(surface_height / game_scale);
  
  __android_log_print(ANDROID_LOG_INFO, g_LogTag, 
                      "Device: %s, Density: %d, Size: %dx%d dp, Scale: %.2f, Physical: %dx%d, Logical: %dx%d", 
                      is_tablet ? "Tablet" : "Phone", density, screen_width_dp, screen_height_dp,
                      game_scale, surface_width, surface_height, logical_width, logical_height);
  
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
        // Don't process game input when menu is open or scoreboard is showing
        if (!game->menu_open && !game->show_all_statboxes && self->ship < 8 && (flags == AMOTION_EVENT_ACTION_MOVE || flags == AMOTION_EVENT_ACTION_POINTER_DOWN || flags == AMOTION_EVENT_ACTION_DOWN)) {
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
            float button_y = logical_screen_height - button_size - 55.0f;  // Slightly higher for padding
            float button_center_y = button_y + button_radius;
            float gun_button_x = logical_screen_width - (button_size * 2) - button_spacing - 50.0f;  // Slightly more left
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
            // Abilities are now horizontal at the very bottom, rotated 90° CCW, growing upward
            if ((flags == AMOTION_EVENT_ACTION_DOWN || flags == AMOTION_EVENT_ACTION_POINTER_DOWN) && !android_input.abilities_triggered) {
              float ability_y = logical_screen_height;  // At screen bottom
              float ability_x_start = logical_screen_width - 250.0f;  // Slightly more left for margin
              float ability_item_width = 30.0f;  // Horizontal spacing
              float ability_item_height = 26.0f;  // Height of rotated icon
              
              // Check if touch is within the ability button area (icons grow upward from bottom)
              if (logical_y >= ability_y - ability_item_height && logical_y <= ability_y && logical_x >= ability_x_start) {
                int item_index = (int)((logical_x - ability_x_start) / ability_item_width);
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
                  null::g_MobileMineMode = android_input.mine_mode_active;
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
        } else if (flags == AMOTION_EVENT_ACTION_POINTER_UP) {
          // Reset ability trigger flag when any pointer is lifted
          // This allows subsequent taps while other fingers remain down
          android_input.abilities_triggered = false;
          // Clear ability actions immediately when finger lifts (for multi-touch)
          if (self->ship < 8) {
            null::g_InputState.SetAction(null::InputAction::Burst, false);
            null::g_InputState.SetAction(null::InputAction::Repel, false);
            null::g_InputState.SetAction(null::InputAction::Decoy, false);
            null::g_InputState.SetAction(null::InputAction::Thor, false);
            null::g_InputState.SetAction(null::InputAction::Brick, false);
            null::g_InputState.SetAction(null::InputAction::Rocket, false);
            null::g_InputState.SetAction(null::InputAction::Portal, false);
            null::g_InputState.SetAction(null::InputAction::Warp, false);
          }
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
            
            // Full screen menu (100% of screen)
            float logical_screen_width = (float)null::g_nullspace.surface_width;
            float logical_screen_height = (float)null::g_nullspace.surface_height;
            float menu_width = logical_screen_width;
            float menu_height = logical_screen_height;
            float menu_x = 0.0f;
            float menu_y = 0.0f;
            
            // Left side ship grid
            float grid_start_x = 20.0f;
            float grid_start_y = 78.0f;
            float cell_width = 110.0f;
            float cell_height = 70.0f;
            float grid_spacing = 8.0f;
            
            // Right column buttons (Scoreboard, Help, Quit)
            float right_button_width = 140.0f;
            float right_button_height = 35.0f;
            float button_margin = 20.0f;
            float right_column_x = menu_width - right_button_width - button_margin;
            float right_start_y = 30.0f;
            
            bool handled_click = false;
            
            // Check ship grid (3x3 grid, 9 cells for 8 ships + spectator)
            if (logical_x >= grid_start_x && logical_x <= grid_start_x + 3 * (cell_width + grid_spacing) &&
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
            
            // Check right column buttons (Scoreboard, Help, Quit)
            if (!handled_click && logical_x >= right_column_x && logical_x <= right_column_x + right_button_width &&
                logical_y >= right_start_y && logical_y <= right_start_y + 3 * (right_button_height + 8.0f)) {
              float relative_y = logical_y - right_start_y;
              int button_index = (int)(relative_y / (right_button_height + 8.0f));
              
              if (button_index == 0) {
                // Scoreboard - toggle multi-statbox display
                game->show_all_statboxes = !game->show_all_statboxes;
                game->menu_open = false;
                handled_click = true;
              } else if (button_index == 1) {
                // Help - show onboarding wizard
                game->onboarding.Show();
                game->menu_open = false;
                handled_click = true;
              } else if (button_index == 2) {
                // Quit
                null::g_InputState.OnCharacter('q');
                handled_click = true;
              }
            }
            
            // Check CLOSE button at bottom right
            float close_button_x = right_column_x;
            float close_button_y = menu_height - right_button_height - button_margin;
            
            if (!handled_click && logical_x >= close_button_x && logical_x <= close_button_x + right_button_width &&
                logical_y >= close_button_y && logical_y <= close_button_y + right_button_height) {
              // Close menu
              null::g_InputState.OnCharacter(NULLSPACE_KEY_ESCAPE);
              handled_click = true;
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
            // Spectator mode tap handling
            else if (self->ship == 8 && is_tap && touch_duration < LONG_PRESS_TIMEOUT) {
              float logical_screen_width = (float)null::g_nullspace.surface_width;
              float logical_screen_height = (float)null::g_nullspace.surface_height;
              
              // Check if tap is on xradar icon (bottom-right area, 26x25px icon at y = height - 35)
              float xradar_x = logical_screen_width - 26.0f;
              float xradar_y = logical_screen_height - 35.0f;
              float xradar_width = 26.0f;
              float xradar_height = 25.0f;
              
              if (logical_x >= xradar_x && logical_x <= logical_screen_width &&
                  logical_y >= xradar_y && logical_y <= xradar_y + xradar_height) {
                // Toggle xradar
                if (null::g_InputState.action_callback) {
                  null::g_InputState.action_callback(null::g_InputState.user, null::InputAction::XRadar);
                }
              } else {
                // Tap elsewhere - follow nearby player or cycle to next
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
                
                // If tap is near a player (within 4 tiles), follow that player
                if (closest && closest_dist <= 4.0f * 4.0f) {
                  game->specview.spectate_id = closest->id;
                  game->specview.spectate_frequency = closest->frequency;
                } else {
                  // Otherwise, cycle to next player in the list
                  null::u16 current_id = game->specview.spectate_id;
                  
                  // Find current player's index in the player array
                  size_t current_index = 0;
                  bool found_current = false;
                  if (current_id != null::kInvalidSpectateId) {
                    for (size_t i = 0; i < game->player_manager.player_count; ++i) {
                      if (game->player_manager.players[i].id == current_id) {
                        current_index = i;
                        found_current = true;
                        break;
                      }
                    }
                  }
                  
                  // Start search from next player (or first player if not following anyone)
                  size_t start_index = found_current ? (current_index + 1) : 0;
                  
                  // Search through all players, wrapping around
                  bool found_next = false;
                  for (size_t offset = 0; offset < game->player_manager.player_count; ++offset) {
                    size_t i = (start_index + offset) % game->player_manager.player_count;
                    null::Player* player = game->player_manager.players + i;
                    if (player->ship == 8) continue;  // Skip spectators
                    game->specview.spectate_id = player->id;
                    game->specview.spectate_frequency = player->frequency;
                    found_next = true;
                    break;
                  }
                }
              }
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

            // If dragging significantly, exit follow mode
            if (abs(offset_x) > 1.0f || abs(offset_y) > 1.0f) {
              game->specview.spectate_id = null::kInvalidSpectateId;
            }
          } else {
            // Ship mode: Virtual D-Pad in bottom-left for flying
            float logical_x = x * (null::g_nullspace.surface_width / (float)screen_width);
            float logical_y = y * (null::g_nullspace.surface_height / (float)screen_height);
            float logical_screen_height = (float)null::g_nullspace.surface_height;
            
            // D-pad position: 20px from left and 20px from bottom (equal padding)
            float dpad_size = 140.0f;  // Increased from 80
            float dpad_radius = dpad_size / 2.0f;
            float dpad_center_x = 20.0f + dpad_radius;  // 20px from left edge
            float button_size = 72.0f;
            float button_y = logical_screen_height - button_size - 55.0f;  // Match weapon buttons
            float dpad_center_y = button_y + button_size / 2.0f - 20.0f;  // 20px higher
            
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

  // Initialize settings storage
  std::string settings_path = std::string(app->activity->internalDataPath) + "/settings.txt";
  null::g_AndroidSettings.file_path = settings_path;
  null::g_AndroidSettings.Load();
  __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Settings initialized at: %s", settings_path.c_str());

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
