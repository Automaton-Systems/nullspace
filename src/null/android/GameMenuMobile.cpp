#ifdef NULLSPACE_MOBILE

#include <null/Game.h>

#include <null/render/Camera.h>
#include <null/render/Graphics.h>
#include <null/render/SpriteRenderer.h>

namespace null {

void Game::RenderMenuAndroid() {
  const char* kRightMenuText[] = {"Scoreboard", "Help", "Quit"};

  const char* kLeftMenuText[] = {"Warbird", "Javelin", "Spider",
                                  "Leviathan", "Terrier",
                                  "Weasel", "Lancaster", "Shark",
                                  "Spectator"};

  // Full screen menu (100% of screen)
  Vector2f dimensions = ui_camera.surface_dim;
  Vector2f half_dimensions = dimensions * 0.5f;
  Vector2f topleft(0, 0);

  SpriteRenderable background = Graphics::GetColor(ColorType::Background, dimensions);
  sprite_renderer.Draw(ui_camera, background, topleft, Layer::TopMost);

  SpriteRenderable separator = Graphics::GetColor(ColorType::Border1, Vector2f(dimensions.x, 1));
  sprite_renderer.Draw(ui_camera, separator, topleft + Vector2f(0, 20), Layer::TopMost);
  Graphics::DrawBorder(sprite_renderer, ui_camera, topleft + half_dimensions, half_dimensions);

  sprite_renderer.DrawText(ui_camera, "-= Menu =-", TextColor::Green, Vector2f(half_dimensions.x, 4),
                           Layer::TopMost, TextAlignment::Center);

  // Left side - ship selection grid with sprites (positioned from left edge)
  float grid_start_x = 20.0f;
  float grid_y = 78.0f;
  float cell_width = 110.0f;
  float cell_height = 70.0f;
  float grid_spacing = 8.0f;
  float total_grid_width = 3 * cell_width + 2 * grid_spacing;

  sprite_renderer.DrawText(ui_camera, "Select Ship", TextColor::DarkRed,
                           Vector2f(grid_start_x + total_grid_width * 0.5f, 44.0f),
                           Layer::TopMost, TextAlignment::Center);

  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kLeftMenuText); ++i) {
    size_t col = i % 3;
    size_t row = i / 3;
    
    Vector2f cell_pos(grid_start_x + col * (cell_width + grid_spacing), grid_y + row * (cell_height + grid_spacing));
    Vector2f cell_center = cell_pos + Vector2f(cell_width * 0.5f, cell_height * 0.5f);

    // Draw cell border
    Graphics::DrawBorder(sprite_renderer, ui_camera, cell_center, Vector2f(cell_width * 0.5f, cell_height * 0.5f));

    // Draw ship sprite (ships are 36x36, 40 rotations per ship)
    // Use ship * 40 for forward-facing sprite
    if (i < 8) {  // Not spectator
      size_t ship_sprite_index = i * 40;
      SpriteRenderable& ship_sprite = Graphics::ship_sprites[ship_sprite_index];
      Vector2f sprite_pos = cell_pos + Vector2f(cell_width * 0.5f - ship_sprite.dimensions.x * 0.5f, 8.0f);
      sprite_renderer.Draw(ui_camera, ship_sprite, sprite_pos, Layer::TopMost);
    }

    // Draw ship name below sprite
    sprite_renderer.DrawText(ui_camera, kLeftMenuText[i], TextColor::White,
                             Vector2f(cell_center.x, cell_pos.y + 52.0f),
                             Layer::TopMost, TextAlignment::Center);
  }

  // Right column - menu buttons (positioned from right edge)
  float button_width = 140.0f;
  float button_height = 35.0f;
  float button_margin = 20.0f;
  float right_column_x = dimensions.x - button_width - button_margin;
  float y = 30.0f;

  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kRightMenuText); ++i) {
    Vector2f button_size(button_width, button_height);
    Vector2f button_pos(right_column_x, y);

    Graphics::DrawBorder(sprite_renderer, ui_camera, button_pos + button_size * 0.5f, button_size * 0.5f);
    sprite_renderer.DrawText(ui_camera, kRightMenuText[i], TextColor::White,
                             Vector2f(button_pos.x + button_size.x * 0.5f, button_pos.y + 10),
                             Layer::TopMost, TextAlignment::Center);

    y += button_height + 8.0f;
  }

  // CLOSE button at bottom right corner
  Vector2f close_button_size(button_width, button_height);
  Vector2f close_button_pos(right_column_x, dimensions.y - button_height - button_margin);
  
  Graphics::DrawBorder(sprite_renderer, ui_camera, close_button_pos + close_button_size * 0.5f, close_button_size * 0.5f);
  sprite_renderer.DrawText(ui_camera, "CLOSE", TextColor::White,
                           Vector2f(close_button_pos.x + close_button_size.x * 0.5f, close_button_pos.y + 10),
                           Layer::TopMost, TextAlignment::Center);

  sprite_renderer.Render(ui_camera);
}

}  // namespace null

#endif  // NULLSPACE_MOBILE
