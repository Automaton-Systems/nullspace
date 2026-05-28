#include "OnboardingWizard.h"

#include <null/Clock.h>
#include <null/PlayerManager.h>
#include <null/render/Camera.h>
#include <null/render/Graphics.h>
#include <null/render/SpriteRenderer.h>

namespace null {

OnboardingWizard::OnboardingWizard(PlayerManager& player_manager)
    : player_manager(player_manager) {
  prize_anim.sprite = &Graphics::anim_prize;
  prize_anim.t = 0.0f;
  flag_anim.sprite = &Graphics::anim_flag;
  flag_anim.t = 0.0f;
}

void OnboardingWizard::Show() {
  active = true;
  current_page = 0;
  animation_time = 0.0f;
}

void OnboardingWizard::Hide() {
  active = false;
}

bool OnboardingWizard::OnTap() {
  if (!active) return false;
  
  current_page++;
  if (current_page >= total_pages) {
    Hide();
  }
  
  return true;
}

void OnboardingWizard::Render(Camera& ui_camera, SpriteRenderer& renderer) {
  if (!active) return;
  
  // Update animations (assuming ~60fps, add 0.016s per frame)
  const float kFixedDT = 1.0f / 60.0f;
  animation_time += kFixedDT;
  prize_anim.t += kFixedDT;
  if (prize_anim.sprite && prize_anim.t >= prize_anim.sprite->duration) {
    prize_anim.t -= prize_anim.sprite->duration;
  }
  flag_anim.t += kFixedDT;
  if (flag_anim.sprite && flag_anim.t >= flag_anim.sprite->duration) {
    flag_anim.t -= flag_anim.sprite->duration;
  }
  
  switch (current_page) {
    case 0: RenderPage1(ui_camera, renderer); break;
    case 1: RenderPage2(ui_camera, renderer); break;
    case 2: RenderPage3(ui_camera, renderer); break;
    case 3: RenderPage4(ui_camera, renderer); break;
  }
  
  RenderPageIndicator(ui_camera, renderer);
}

void OnboardingWizard::RenderPage1(Camera& ui_camera, SpriteRenderer& renderer) {
  // Full screen overlay
  Vector2f screen_center = ui_camera.surface_dim * 0.5f;
  Vector2f half_screen = ui_camera.surface_dim * 0.5f;
  
  SpriteRenderable background = Graphics::GetColor(ColorType::Background, ui_camera.surface_dim);
  renderer.Draw(ui_camera, background, Vector2f(0, 0), Layer::TopMost);
  
  // Title
  renderer.DrawText(ui_camera, "Ships & Abilities", TextColor::Green, 
                   Vector2f(screen_center.x, 20), Layer::TopMost, TextAlignment::Center);
  
  // Main text
  float text_y = 50;
  renderer.DrawText(ui_camera, "Each ship has unique abilities (such as cloaking)", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 20;
  renderer.DrawText(ui_camera, "Select your ship in the MENU to begin or change anytime", TextColor::Yellow,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  
  // Draw all 8 ships in a grid (3 columns)
  const char* ship_names[] = {"Warbird", "Javelin", "Spider", "Leviathan", 
                             "Terrier", "Weasel", "Lancaster", "Shark"};
  
  float grid_y = text_y + 30;
  float cell_width = 80.0f;
  float cell_height = 60.0f;
  float spacing = 8.0f;
  float grid_width = 3 * cell_width + 2 * spacing;
  float grid_start_x = screen_center.x - grid_width * 0.5f;
  
  for (size_t i = 0; i < 8; ++i) {
    size_t col = i % 3;
    size_t row = i / 3;
    
    Vector2f cell_pos(grid_start_x + col * (cell_width + spacing), 
                     grid_y + row * (cell_height + spacing));
    Vector2f cell_center = cell_pos + Vector2f(cell_width * 0.5f, cell_height * 0.5f);
    
    // Draw ship sprite (forward-facing)
    size_t ship_index = i * 40;
    SpriteRenderable& ship_sprite = Graphics::ship_sprites[ship_index];
    Vector2f sprite_pos = cell_pos + Vector2f(cell_width * 0.5f - ship_sprite.dimensions.x * 0.5f, 4.0f);
    renderer.Draw(ui_camera, ship_sprite, sprite_pos, Layer::TopMost);
    
    // Draw ship name
    renderer.DrawText(ui_camera, ship_names[i], TextColor::White,
                     Vector2f(cell_center.x, cell_pos.y + 42.0f), Layer::TopMost, TextAlignment::Center);
  }
  
  // Bottom instruction
  renderer.DrawText(ui_camera, "Tap anywhere to continue", TextColor::DarkRed,
                   Vector2f(screen_center.x, ui_camera.surface_dim.y - 30), Layer::TopMost, TextAlignment::Center);
}

void OnboardingWizard::RenderPage2(Camera& ui_camera, SpriteRenderer& renderer) {
  Vector2f screen_center = ui_camera.surface_dim * 0.5f;
  
  SpriteRenderable background = Graphics::GetColor(ColorType::Background, ui_camera.surface_dim);
  renderer.Draw(ui_camera, background, Vector2f(0, 0), Layer::TopMost);
  
  // Title
  renderer.DrawText(ui_camera, "Power-Ups", TextColor::Green,
                   Vector2f(screen_center.x, 20), Layer::TopMost, TextAlignment::Center);
  
  // Main text
  float text_y = 60;
  renderer.DrawText(ui_camera, "Collect green prizes to power up", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 20;
  renderer.DrawText(ui_camera, "Get better guns, bombs, and new abilities", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 20;
  renderer.DrawText(ui_camera, "Die = lose all powerups", TextColor::DarkRed,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  
  // Draw animated green prize (scaled up 2x for visibility)
  text_y += 60;
  SpriteRenderable& prize_sprite = prize_anim.GetFrame();
  
  // Center the prize and scale it up 2x
  float scale = 2.0f;
  Vector2f scaled_size = prize_sprite.dimensions * scale;
  Vector2f prize_pos = Vector2f(screen_center.x - scaled_size.x * 0.5f, text_y);
  
  // Draw single animated sprite scaled up
  // We'll use a simple approach: draw it once at 4x the UV coordinates
  // Actually, let's just draw it large by manipulating the dimensions
  SpriteRenderable large_prize = prize_sprite;
  large_prize.dimensions = scaled_size;
  renderer.Draw(ui_camera, large_prize, prize_pos, Layer::TopMost);
  
  text_y += scaled_size.y + 20;
  renderer.DrawText(ui_camera, "Green Prize", TextColor::Green,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  
  // Bottom instruction
  renderer.DrawText(ui_camera, "Tap anywhere to continue", TextColor::DarkRed,
                   Vector2f(screen_center.x, ui_camera.surface_dim.y - 30), Layer::TopMost, TextAlignment::Center);
}

void OnboardingWizard::RenderPage3(Camera& ui_camera, SpriteRenderer& renderer) {
  Vector2f screen_center = ui_camera.surface_dim * 0.5f;
  
  SpriteRenderable background = Graphics::GetColor(ColorType::Background, ui_camera.surface_dim);
  renderer.Draw(ui_camera, background, Vector2f(0, 0), Layer::TopMost);
  
  // Title
  renderer.DrawText(ui_camera, "Weapons & Controls", TextColor::Green,
                   Vector2f(screen_center.x, 20), Layer::TopMost, TextAlignment::Center);
  
  // Main text
  float text_y = 50;
  renderer.DrawText(ui_camera, "Tap icons on the screen edges to use abilities", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 20;
  renderer.DrawText(ui_camera, "Abilities use energy, using many at once drains faster", TextColor::Yellow,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 30;
  
  // Left side items with icons
  float icon_x = 10;
  float text_x = icon_x + 50;
  float y = text_y;
  
  // Burst
  renderer.Draw(ui_camera, Graphics::icon_sprites[30], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Burst - shoots in all directions", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Repel
  renderer.Draw(ui_camera, Graphics::icon_sprites[31], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Repel - pushes away enemies and shots", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Decoy
  renderer.Draw(ui_camera, Graphics::icon_sprites[40], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Decoy - spawns a decoy ship", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Thor
  renderer.Draw(ui_camera, Graphics::icon_sprites[41], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Thor - missile goes through walls", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Brick
  renderer.Draw(ui_camera, Graphics::icon_sprites[42], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Brick - creates a wall", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Rocket
  renderer.Draw(ui_camera, Graphics::icon_sprites[43], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Rocket - boost forward fast", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Portal
  renderer.Draw(ui_camera, Graphics::icon_sprites[46], Vector2f(icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Portal - drop portal and tap to teleport", TextColor::White,
                   Vector2f(text_x, y + 5), Layer::TopMost);
  y += 40;
  
  // D-pad section (moved down more for gap)
  renderer.DrawText(ui_camera, "D-Pad: Move (drag beyond circle = afterburner 'AB')", TextColor::Yellow,
                   Vector2f(30, y), Layer::TopMost);
  
  // Right side items
  float right_icon_x = ui_camera.surface_dim.x - 360;
  float right_text_x = right_icon_x + 50;
  y = text_y;
  
  // Gun
  renderer.Draw(ui_camera, Graphics::icon_sprites[28], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Gun - toggle weapon fire type", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Bomb
  renderer.Draw(ui_camera, Graphics::icon_sprites[29], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Bomb - toggle mines or bombs", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Stealth
  renderer.Draw(ui_camera, Graphics::icon_sprites[32], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Stealth - invisible on radar", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Cloak
  renderer.Draw(ui_camera, Graphics::icon_sprites[34], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Cloak - invisible on screen", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // XRadar
  renderer.Draw(ui_camera, Graphics::icon_sprites[36], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "XRadar - see cloaked ships", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  y += 25;
  
  // Antiwarp
  renderer.Draw(ui_camera, Graphics::icon_sprites[38], Vector2f(right_icon_x, y), Layer::TopMost);
  renderer.DrawText(ui_camera, "Antiwarp - blocks warping", TextColor::White,
                   Vector2f(right_text_x, y + 5), Layer::TopMost);
  
  // Bottom instruction
  renderer.DrawText(ui_camera, "Tap anywhere to continue", TextColor::DarkRed,
                   Vector2f(screen_center.x, ui_camera.surface_dim.y - 30), Layer::TopMost, TextAlignment::Center);
}

void OnboardingWizard::RenderPage4(Camera& ui_camera, SpriteRenderer& renderer) {
  Vector2f screen_center = ui_camera.surface_dim * 0.5f;
  
  SpriteRenderable background = Graphics::GetColor(ColorType::Background, ui_camera.surface_dim);
  renderer.Draw(ui_camera, background, Vector2f(0, 0), Layer::TopMost);
  
  // Title
  renderer.DrawText(ui_camera, "Game Modes", TextColor::Green,
                   Vector2f(screen_center.x, 20), Layer::TopMost, TextAlignment::Center);
  
  float text_y = 60;
  renderer.DrawText(ui_camera, "Arenas have different modes:", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 30;
  
  renderer.DrawText(ui_camera, "Team Deathmatch - eliminate enemies", TextColor::White,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  text_y += 25;
  
  renderer.DrawText(ui_camera, "Capture the Flag - get the flag, keep it for your team", TextColor::Yellow,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  
  // Draw animated flag sprite (scaled up 2x for visibility)
  text_y += 50;
  SpriteRenderable& flag_sprite = flag_anim.GetFrame();
  
  // Center the flag and scale it up 2x
  float scale = 2.0f;
  Vector2f scaled_size = flag_sprite.dimensions * scale;
  Vector2f flag_pos = Vector2f(screen_center.x - scaled_size.x * 0.5f, text_y);
  
  // Draw single animated sprite scaled up
  SpriteRenderable large_flag = flag_sprite;
  large_flag.dimensions = scaled_size;
  renderer.Draw(ui_camera, large_flag, flag_pos, Layer::TopMost);
  
  text_y += scaled_size.y + 20;
  renderer.DrawText(ui_camera, "Flag", TextColor::Yellow,
                   Vector2f(screen_center.x, text_y), Layer::TopMost, TextAlignment::Center);
  
  // Bottom instruction
  renderer.DrawText(ui_camera, "Tap to start playing!", TextColor::Green,
                   Vector2f(screen_center.x, ui_camera.surface_dim.y - 30), Layer::TopMost, TextAlignment::Center);
}

void OnboardingWizard::RenderPageIndicator(Camera& ui_camera, SpriteRenderer& renderer) {
  // Draw page dots at bottom
  float dot_y = ui_camera.surface_dim.y - 50;
  float dot_spacing = 16;
  float total_width = (total_pages - 1) * dot_spacing;
  float start_x = ui_camera.surface_dim.x * 0.5f - total_width * 0.5f;
  
  for (int i = 0; i < total_pages; ++i) {
    ColorType color = (i == current_page) ? ColorType::RadarSelf : ColorType::Border1;
    SpriteRenderable dot = Graphics::GetColor(color, Vector2f(8, 8));
    renderer.Draw(ui_camera, dot, Vector2f(start_x + i * dot_spacing, dot_y), Layer::TopMost);
  }
}

}  // namespace null
