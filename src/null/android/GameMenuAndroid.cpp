#ifdef __ANDROID__

#include <null/Game.h>

#include <null/render/Camera.h>
#include <null/render/Graphics.h>
#include <null/render/SpriteRenderer.h>

namespace null {

void Game::RenderMenuAndroid() {
  const char* kLeftMenuText[] = {"Quit",        "Help",          "Stat Box",
                                 "Name tags",   "Radar",         "Messages",
                                 "Help ticker", "Engine sounds", "Arena List",
                                 "Set Banner",  "Ignore macros", "Adjust stat box"};

  const char* kRightMenuText[] = {"Warbird", "Javelin", "Spider",
                                  "Leviathan", "Terrier",
                                  "Weasel", "Lancaster", "Shark",
                                  "Spectator"};

  Vector2f dimensions(420.0f, 240.0f);
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
  float button_width = 190.0f;
  float button_height = 16.0f;
  float column_spacing = 10.0f;

  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kLeftMenuText); ++i) {
    Vector2f button_size(button_width, button_height);
    Vector2f button_pos(topleft.x + 5, topleft.y + y);

    Graphics::DrawBorder(sprite_renderer, ui_camera, button_pos + button_size * 0.5f, button_size * 0.5f);
    sprite_renderer.DrawText(ui_camera, kLeftMenuText[i], TextColor::White,
                             Vector2f(button_pos.x + button_size.x * 0.5f, button_pos.y + 4),
                             Layer::TopMost, TextAlignment::Center);

    y += button_height + 2.0f;
  }

  sprite_renderer.DrawText(ui_camera, "Any other key to resume game", TextColor::Yellow,
                           Vector2f(topleft.x + half_dimensions.x, y + 2), Layer::TopMost, TextAlignment::Center);

  float right_x = topleft.x + button_width + column_spacing + 10;
  y = 32.0f;

  sprite_renderer.DrawText(ui_camera, "Ships", TextColor::DarkRed,
                           Vector2f(right_x + button_width * 0.5f, 20.0f),
                           Layer::TopMost, TextAlignment::Center);

  for (size_t i = 0; i < NULLSPACE_ARRAY_SIZE(kRightMenuText); ++i) {
    Vector2f button_size(button_width, button_height);
    Vector2f button_pos(right_x, topleft.y + y);

    Graphics::DrawBorder(sprite_renderer, ui_camera, button_pos + button_size * 0.5f, button_size * 0.5f);
    sprite_renderer.DrawText(ui_camera, kRightMenuText[i], TextColor::White,
                             Vector2f(button_pos.x + button_size.x * 0.5f, button_pos.y + 4),
                             Layer::TopMost, TextAlignment::Center);

    y += button_height + 2.0f;
  }

  sprite_renderer.Render(ui_camera);
}

}  // namespace null

#endif  // __ANDROID__
