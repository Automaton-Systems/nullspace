#ifdef __ANDROID__

#include <null/Game.h>

#include <null/render/Camera.h>
#include <null/render/Graphics.h>
#include <null/render/SpriteRenderer.h>

namespace null {

void Game::RenderMenuAndroid() {
  const char* kLeftMenuText[] = {"Quit", "Scoreboard", "Help"};

  const char* kRightMenuText[] = {"Warbird", "Javelin", "Spider",
                                  "Leviathan", "Terrier",
                                  "Weasel", "Lancaster", "Shark",
                                  "Spectator"};

  Vector2f dimensions(620.0f, 340.0f);
  Vector2f half_dimensions = dimensions * 0.5f;
  Vector2f topleft((ui_camera.surface_dim.x - dimensions.x) * 0.5f, 3);

  SpriteRenderable background = Graphics::GetColor(ColorType::Background, dimensions);
  sprite_renderer.Draw(ui_camera, background, topleft, Layer::TopMost);

  SpriteRenderable separator = Graphics::GetColor(ColorType::Border1, Vector2f(dimensions.x, 1));
  sprite_renderer.Draw(ui_camera, separator, topleft + Vector2f(0, 13), Layer::TopMost);
  Graphics::DrawBorder(sprite_renderer, ui_camera, topleft + half_dimensions, half_dimensions);

  sprite_renderer.DrawText(ui_camera, "-= Menu =-", TextColor::Green, Vector2f(topleft.x + half_dimensions.x, 4),
                           Layer::TopMost, TextAlignment::Center);

  float y = 20.0f;
  float button_width = 140.0f;
  float button_height = 35.0f;
  float column_spacing = 20.0f;

  // Left column - simplified menu
  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kLeftMenuText); ++i) {
    Vector2f button_size(button_width, button_height);
    Vector2f button_pos(topleft.x + 10, topleft.y + y);

    Graphics::DrawBorder(sprite_renderer, ui_camera, button_pos + button_size * 0.5f, button_size * 0.5f);
    sprite_renderer.DrawText(ui_camera, kLeftMenuText[i], TextColor::White,
                             Vector2f(button_pos.x + button_size.x * 0.5f, button_pos.y + 10),
                             Layer::TopMost, TextAlignment::Center);

    y += button_height + 8.0f;
  }

  // Right side - ship selection grid with sprites
  float grid_start_x = topleft.x + button_width + column_spacing + 20;
  float grid_y = topleft.y + 40.0f;
  float cell_width = 110.0f;
  float cell_height = 70.0f;
  float grid_spacing = 8.0f;
  float total_grid_width = 3 * cell_width + 2 * grid_spacing;

  sprite_renderer.DrawText(ui_camera, "Ships", TextColor::DarkRed,
                           Vector2f(grid_start_x + total_grid_width * 0.5f, 20.0f),
                           Layer::TopMost, TextAlignment::Center);

  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kRightMenuText); ++i) {
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
    sprite_renderer.DrawText(ui_camera, kRightMenuText[i], TextColor::White,
                             Vector2f(cell_center.x, cell_pos.y + 52.0f),
                             Layer::TopMost, TextAlignment::Center);
  }

  sprite_renderer.Render(ui_camera);
}

}  // namespace null

#endif  // __ANDROID__
