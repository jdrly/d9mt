#pragma once

// The app profile (ADR-0008, D9MT_APP_PROFILE in docs/configuration.md): which game's tuning d9mt
// applies. The game's exe selects it: swtor.exe gets `swtor`, every other exe `generic`.
// A change for SWTOR alone adds its field to AppProfile, with the generic value as the default,
// sets the SWTOR value in appProfileFor, and gets a D9MT_<FIELD>=0|1 override of its own
// (applyAppProfileSwitch), so an A/B is one binary with the field toggled.
//
// Header-only and dependency-free so the exe match and the override are unit-tested natively
// (test/native/app_profile_test.cpp). MtlInstance reads the exe path and the variable once, at
// Direct3DCreate9 (mtl_device_stub.cpp), and logs the choice.

#include <cstdint>
#include <string>

namespace dxvk {

  enum class AppProfileId : uint32_t { Generic, Swtor };

  // What D9MT_APP_PROFILE, or a field's D9MT_<FIELD>, did: unset or empty, chose the app profile
  // (set the field), or named none (ignored).
  enum class AppProfileOverride : uint32_t { Unset, Used, Unknown };

  struct AppProfile {
    AppProfileId id = AppProfileId::Generic;
    // Static buffers (DEFAULT pool, WRITEONLY without DYNAMIC) mapped directly on every memory
    // profile, demoted to staging after the same in-place locks as below 64 GB of RAM: the
    // `unlimited` memory profile otherwise puts them on the staging path
    // (applyStaticBuffersDirect, mtl_memory_profile.h). swtor: on. D9MT_STATIC_BUFFERS_DIRECT.
    bool staticBuffersDirect = false;
    // A part-surface depth/stencil clear of the open pass's own depth-stencil attachment is drawn
    // inside that pass instead of ending it (#129). swtor: on. D9MT_INPASS_CLEAR.
    bool inpassClear = false;
  };

  // The fields' values of an app profile.
  inline AppProfile appProfileFor(AppProfileId id) {
    AppProfile p;
    p.id = id;
    p.staticBuffersDirect = id == AppProfileId::Swtor;
    p.inpassClear         = id == AppProfileId::Swtor;
    return p;
  }

  // A field's D9MT_<FIELD> override: "1" sets it, "0" clears it, empty is unset, anything else is
  // ignored (Unknown) and the app profile's value stays.
  inline AppProfileOverride applyAppProfileSwitch(const std::string& value, bool& field) {
    if (value.empty())
      return AppProfileOverride::Unset;
    if (value != "0" && value != "1")
      return AppProfileOverride::Unknown;
    field = value == "1";
    return AppProfileOverride::Used;
  }

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

  // The app profile an exe selects: swtor for a file named swtor.exe in any case, generic for any
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

  // The override when it names an app profile, otherwise the exe's. An empty value is unset; any
  // other value that names no app profile is ignored and reported in `how`.
  inline AppProfile resolveAppProfile(const std::string& exePath, const std::string& value,
                                      AppProfileOverride& how) {
    AppProfileId id = appProfileForExe(exePath);
    how = AppProfileOverride::Unset;
    if (!value.empty())
      how = parseAppProfile(value, id) ? AppProfileOverride::Used : AppProfileOverride::Unknown;
    return appProfileFor(id);
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

  // The fields' log line: `d9mt: app profile fields: staticBuffersDirect on, inpassClear on`,
  // with ` (D9MT_<FIELD>)` after a value its override set.
  inline std::string appProfileFieldsLine(const AppProfile& p, AppProfileOverride staticBuffersDirect,
                                          AppProfileOverride inpassClear = AppProfileOverride::Unset) {
    return std::string("d9mt: app profile fields: staticBuffersDirect ")
      + (p.staticBuffersDirect ? "on" : "off")
      + (staticBuffersDirect == AppProfileOverride::Used ? " (D9MT_STATIC_BUFFERS_DIRECT)" : "")
      + ", inpassClear " + (p.inpassClear ? "on" : "off")
      + (inpassClear == AppProfileOverride::Used ? " (D9MT_INPASS_CLEAR)" : "");
  }

}
