#include "AndroidSettings.h"

#ifdef __ANDROID__
#include <fstream>
#include <sstream>
#include "../Logger.h"

namespace null {

AndroidSettings g_AndroidSettings;

void AndroidSettings::Load() {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    Log(LogLevel::Info, "No settings file found, using defaults");
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos) continue;

    std::string key = line.substr(0, equals_pos);
    std::string value = line.substr(equals_pos + 1);

    if (key == "username") {
      username = value;
    } else if (key == "wizard_shown") {
      wizard_shown = (value == "1" || value == "true");
    }
  }

  file.close();
  Log(LogLevel::Info, "Settings loaded: username='%s', wizard_shown=%d", 
      username.c_str(), wizard_shown);
}

void AndroidSettings::Save() {
  std::ofstream file(file_path);
  if (!file.is_open()) {
    Log(LogLevel::Error, "Failed to save settings to: %s", file_path.c_str());
    return;
  }

  file << "username=" << username << "\n";
  file << "wizard_shown=" << (wizard_shown ? "1" : "0") << "\n";

  file.close();
  Log(LogLevel::Info, "Settings saved");
}

void AndroidSettings::SetUsername(const std::string& name) {
  username = name;
  Save();
}

void AndroidSettings::SetWizardShown(bool shown) {
  wizard_shown = shown;
  Save();
}

}  // namespace null

#endif  // __ANDROID__
