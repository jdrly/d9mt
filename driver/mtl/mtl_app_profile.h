#pragma once

// The app profile (ADR-0008, D9MT_APP_PROFILE in docs/configuration.md): which game's tuning d9mt
// applies. The game's exe selects it: swtor.exe gets `swtor`, every other exe `generic`.
// Both profiles hold the same values today, so no game behaves differently yet. A change for
// SWTOR alone adds its field to AppProfile, with the generic value as the default, and sets the
// SWTOR value where the profile is resolved.
//
// Header-only and dependency-free so the exe match and the override are unit-tested natively
// (test/native/app_profile_test.cpp). MtlInstance reads the exe path and the variable once, at
// Direct3DCreate9 (mtl_device_stub.cpp), and logs the choice.

#include <cstdint>
#include <string>

namespace dxvk {

  enum class AppProfileId : uint32_t { Generic, Swtor };

  struct AppProfile {
    AppProfileId id = AppProfileId::Generic;
  };

  // What D9MT_APP_PROFILE did: unset or empty, chose the profile, or named no profile (ignored).
  enum class AppProfileOverride : uint32_t { Unset, Used, Unknown };

  inline const char* appProfileName(AppProfileId id) {
    return id == AppProfileId::Swtor ? "swtor" : "generic";
  }

  inline std::string appProfileLower(std::string s) {
    for (char& c : s)
      c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return s;
  }

  // The file name of an exe path: what follows the last '\' or '/'. Empty for an empty path.
  inline std::string appProfileExeName(const std::string& exePath) {
    const size_t n = exePath.find_last_of("\\/");
    return n == std::string::npos ? exePath : exePath.substr(n + 1);
  }

  // The profile an exe selects: swtor for a file named swtor.exe in any case, generic for any
  // other name, and for an empty path (the exe's path couldn't be read).
  inline AppProfileId appProfileForExe(const std::string& exePath) {
    return appProfileLower(appProfileExeName(exePath)) == "swtor.exe"
      ? AppProfileId::Swtor : AppProfileId::Generic;
  }

  // D9MT_APP_PROFILE's value, `generic` or `swtor` in any case. Anything else: false.
  inline bool parseAppProfile(const std::string& value, AppProfileId& id) {
    const std::string s = appProfileLower(value);
    if (s == "generic")    id = AppProfileId::Generic;
    else if (s == "swtor") id = AppProfileId::Swtor;
    else                   return false;
    return true;
  }

  // The override when it names a profile, otherwise the exe's profile. An empty value is unset;
  // any other value that names no profile is ignored and reported in `how`.
  inline AppProfile resolveAppProfile(const std::string& exePath, const std::string& value,
                                      AppProfileOverride& how) {
    AppProfile p;
    p.id = appProfileForExe(exePath);
    how = AppProfileOverride::Unset;
    if (!value.empty())
      how = parseAppProfile(value, p.id) ? AppProfileOverride::Used : AppProfileOverride::Unknown;
    return p;
  }

  // The start-up log line: `d9mt: app profile swtor (exe swtor.exe)`, with `D9MT_APP_PROFILE, `
  // before `exe` when the override chose it. Only the exe's file name: the rest of the path can
  // hold the player's user name.
  inline std::string appProfileLogLine(const AppProfile& p, const std::string& exePath,
                                       AppProfileOverride how) {
    const std::string exe = appProfileExeName(exePath);
    return std::string("d9mt: app profile ") + appProfileName(p.id) + " ("
      + (how == AppProfileOverride::Used ? "D9MT_APP_PROFILE, " : "")
      + "exe " + (exe.empty() ? "unknown" : exe) + ")";
  }

}
