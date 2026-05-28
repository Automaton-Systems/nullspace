#pragma once

#include <string>

namespace null {

struct AndroidSettings {
  std::string username;
  bool wizard_shown = false;
  std::string file_path;

  void Load();
  void Save();
  void SetUsername(const std::string& name);
  void SetWizardShown(bool shown);
};

#ifdef __ANDROID__
extern AndroidSettings g_AndroidSettings;
#endif

}  // namespace null
