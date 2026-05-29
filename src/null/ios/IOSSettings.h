#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <string>

namespace null {

struct IOSSettings {
  std::string username;
  bool wizard_shown = false;

  void Load();
  void Save();
  void SetUsername(const std::string& name);
  void SetWizardShown(bool shown);
};

#if defined(TARGET_OS_IOS) && TARGET_OS_IOS
extern IOSSettings g_IOSSettings;
#endif

}  // namespace null
