#include "IOSSettings.h"

#if TARGET_OS_IOS

#import <Foundation/Foundation.h>
#include "../Logger.h"

namespace null {

IOSSettings g_IOSSettings;

static NSString* const kUsernameKey = @"username";
static NSString* const kWizardShownKey = @"wizard_shown";

void IOSSettings::Load() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* saved_name = [defaults stringForKey:kUsernameKey];
  if (saved_name) {
    username = std::string([saved_name UTF8String]);
  }
  wizard_shown = [defaults boolForKey:kWizardShownKey];
  Log(LogLevel::Info, "IOSSettings loaded: username='%s', wizard_shown=%d", username.c_str(), wizard_shown);
}

void IOSSettings::Save() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setObject:[NSString stringWithUTF8String:username.c_str()] forKey:kUsernameKey];
  [defaults setBool:wizard_shown forKey:kWizardShownKey];
  Log(LogLevel::Info, "IOSSettings saved");
}

void IOSSettings::SetUsername(const std::string& name) {
  username = name;
  Save();
}

void IOSSettings::SetWizardShown(bool shown) {
  wizard_shown = shown;
  Save();
}

}  // namespace null

#endif  // TARGET_OS_IOS
