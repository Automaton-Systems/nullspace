#ifndef NULLSPACE_ONBOARDINGWIZARD_H_
#define NULLSPACE_ONBOARDINGWIZARD_H_

#include <null/Types.h>
#include <null/render/Animation.h>

namespace null {

struct Camera;
struct SpriteRenderer;
struct PlayerManager;

struct OnboardingWizard {
  PlayerManager& player_manager;
  
  bool active = false;
  int current_page = 0;
  int total_pages = 4;
  float animation_time = 0.0f;
  Animation prize_anim;
  Animation flag_anim;
  
  OnboardingWizard(PlayerManager& player_manager);
  
  void Show();
  void Hide();
  bool IsActive() const { return active; }
  
  // Returns true if tap was consumed
  bool OnTap();
  
  void Render(Camera& ui_camera, SpriteRenderer& renderer);
  
private:
  void RenderPage1(Camera& ui_camera, SpriteRenderer& renderer);
  void RenderPage2(Camera& ui_camera, SpriteRenderer& renderer);
  void RenderPage3(Camera& ui_camera, SpriteRenderer& renderer);
  void RenderPage4(Camera& ui_camera, SpriteRenderer& renderer);
  
  void RenderPageIndicator(Camera& ui_camera, SpriteRenderer& renderer);
};

}  // namespace null

#endif
