// Native unit test for the app profile (ADR-0008; no Wine, no Metal):
//   clang++ -std=c++17 -I driver/mtl test/native/app_profile_test.cpp -o /tmp/apt && /tmp/apt
//
// One table of exe paths and D9MT_APP_PROFILE values with the app profile and the override's
// effect each must give. resolveAppProfile must pass it. Stubs that get one rule wrong run through the
// same table and must fail it: a table they passed wouldn't test that rule.
#include "mtl_app_profile.h"

#include <cstdio>
#include <string>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

using Resolve = AppProfile (*)(const std::string&, const std::string&, AppProfileOverride&);

constexpr AppProfileId G = AppProfileId::Generic, S = AppProfileId::Swtor;
constexpr AppProfileOverride Unset = AppProfileOverride::Unset, Used = AppProfileOverride::Used,
                             Unknown = AppProfileOverride::Unknown;

struct Case {
  const char*        exePath;
  const char*        value;   // D9MT_APP_PROFILE, "" = unset
  AppProfileId       id;
  AppProfileOverride how;
};

static const Case kCases[] = {
  // The exe's file name, in any case, under any folder, with either slash.
  { "C:\\Games\\swtor.exe",               "", S, Unset },
  { "C:\\Games\\SWTOR.EXE",               "", S, Unset },
  { "C:\\Games\\Swtor.Exe",               "", S, Unset },
  { "Z:/Users/player/Games/swtor.exe",    "", S, Unset },
  { "swtor.exe",                          "", S, Unset },
  { "C:\\SWTOR\\code_clear.exe",          "", G, Unset },  // the folder's name doesn't count
  { "C:\\Games\\notswtor.exe",            "", G, Unset },
  { "C:\\Games\\swtor.exe.bak",           "", G, Unset },
  { "C:\\Games\\swtor_x.exe",             "", G, Unset },
  { "C:\\Games\\swtor",                   "", G, Unset },
  { "C:\\Games\\swtor.exe\\",             "", G, Unset },
  { "",                                   "", G, Unset },  // the path couldn't be read
  // The override wins when it names an app profile, in any case.
  { "C:\\Games\\code_clear.exe",     "swtor", S, Used },
  { "C:\\Games\\code_clear.exe",     "SWTOR", S, Used },
  { "C:\\Games\\swtor.exe",        "generic", G, Used },
  { "C:\\Games\\swtor.exe",        "Generic", G, Used },
  { "",                              "swtor", S, Used },
  // Any other value is ignored: the exe decides.
  { "C:\\Games\\swtor.exe",          "bogus", S, Unknown },
  { "C:\\Games\\code_clear.exe",     "bogus", G, Unknown },
  { "C:\\Games\\swtor.exe",         " swtor", S, Unknown },
  { "C:\\Games\\code_clear.exe",   "swtor.exe", G, Unknown },
  { "C:\\Games\\code_clear.exe",         "1", G, Unknown },
};

// Whether `resolve` gives every case's app profile and override effect; prints the first miss.
static bool passes(const char* name, Resolve resolve) {
  for (const Case& c : kCases) {
    AppProfileOverride how = AppProfileOverride::Used;
    const AppProfile p = resolve(c.exePath, c.value, how);
    if (p.id != c.id || how != c.how) {
      std::printf("%-14s \"%s\" with \"%s\": %s, override %u (want %s, %u)\n", name, c.exePath, c.value,
        appProfileName(p.id), unsigned(how), appProfileName(c.id), unsigned(c.how));
      return false;
    }
  }
  std::printf("%-14s passes all %zu cases\n", name, sizeof(kCases) / sizeof(kCases[0]));
  return true;
}

// The stubs: each gets one rule wrong.
static AppProfile alwaysGeneric(const std::string&, const std::string&, AppProfileOverride& how) {
  how = AppProfileOverride::Unset;
  return AppProfile();
}

static AppProfile caseSensitive(const std::string& path, const std::string& value, AppProfileOverride& how) {
  AppProfile p = resolveAppProfile(path, value, how);
  if (how != AppProfileOverride::Used)
    p.id = appProfileExeName(path) == "swtor.exe" ? S : G;
  return p;
}

static AppProfile substring(const std::string& path, const std::string& value, AppProfileOverride& how) {
  AppProfile p = resolveAppProfile(path, value, how);
  if (how != AppProfileOverride::Used)
    p.id = appProfileLower(path).find("swtor") != std::string::npos ? S : G;
  return p;
}

static AppProfile wholePath(const std::string& path, const std::string& value, AppProfileOverride& how) {
  AppProfile p = resolveAppProfile(path, value, how);
  if (how != AppProfileOverride::Used)
    p.id = appProfileLower(path) == "swtor.exe" ? S : G;
  return p;
}

static AppProfile overrideIgnored(const std::string& path, const std::string&, AppProfileOverride& how) {
  return resolveAppProfile(path, "", how);
}

static AppProfile unknownIsGeneric(const std::string& path, const std::string& value, AppProfileOverride& how) {
  AppProfile p = resolveAppProfile(path, value, how);
  if (how == AppProfileOverride::Unknown)
    p.id = G;
  return p;
}

int main() {
  CHECK(passes("resolve", resolveAppProfile));

  // Controls: the same table fails each stub.
  CHECK(!passes("always generic", alwaysGeneric));
  CHECK(!passes("case-sensitive", caseSensitive));
  CHECK(!passes("substring", substring));
  CHECK(!passes("whole path", wholePath));
  CHECK(!passes("no override", overrideIgnored));
  CHECK(!passes("unknown=generic", unknownIsGeneric));

  // The names round-trip.
  for (AppProfileId id : { G, S }) {
    AppProfileId back = id == G ? S : G;
    CHECK(parseAppProfile(appProfileName(id), back) && back == id);
  }

  // The start-up log line: the exe's file name only, never its folders.
  AppProfileOverride how = Unset;
  AppProfile p = resolveAppProfile("C:\\Users\\player\\Games\\swtor.exe", "", how);
  CHECK(appProfileLogLine(p, "C:\\Users\\player\\Games\\swtor.exe", how)
        == "d9mt: app profile swtor (exe swtor.exe)");
  p = resolveAppProfile("C:\\Games\\code_clear.exe", "", how);
  CHECK(appProfileLogLine(p, "C:\\Games\\code_clear.exe", how)
        == "d9mt: app profile generic (exe code_clear.exe)");
  p = resolveAppProfile("C:\\Games\\code_clear.exe", "swtor", how);
  CHECK(appProfileLogLine(p, "C:\\Games\\code_clear.exe", how)
        == "d9mt: app profile swtor (D9MT_APP_PROFILE, exe code_clear.exe)");
  p = resolveAppProfile("C:\\Games\\code_clear.exe", "bogus", how);
  CHECK(appProfileLogLine(p, "C:\\Games\\code_clear.exe", how)
        == "d9mt: app profile generic (exe code_clear.exe)");
  p = resolveAppProfile("", "", how);
  CHECK(appProfileLogLine(p, "", how) == "d9mt: app profile generic (exe unknown)");

  // Fields: swtor's values, generic's defaults, and the resolved app profile carries them.
  CHECK(appProfileFor(S).staticBuffersDirect && !appProfileFor(G).staticBuffersDirect);
  CHECK(!AppProfile().staticBuffersDirect);
  CHECK(appProfileFor(S).inpassClear && !appProfileFor(G).inpassClear && !AppProfile().inpassClear);
  for (const Case& c : kCases) {
    AppProfileOverride h = Unset;
    const AppProfile r = resolveAppProfile(c.exePath, c.value, h);
    CHECK(r.staticBuffersDirect == (r.id == S));
    CHECK(r.inpassClear == (r.id == S));
  }

  // A field's override: 1 and 0 set it either way on either app profile; empty and anything
  // else leave it.
  for (AppProfileId id : { G, S }) {
    const bool def = appProfileFor(id).staticBuffersDirect;
    struct { const char* v; bool want; AppProfileOverride how; } sw[] = {
      { "1", true, Used }, { "0", false, Used }, { "", def, Unset },
      { "true", def, Unknown }, { "on", def, Unknown }, { " 1", def, Unknown }, { "2", def, Unknown },
    };
    for (const auto& s : sw) {
      AppProfile f = appProfileFor(id);
      CHECK(applyAppProfileSwitch(s.v, f.staticBuffersDirect) == s.how);
      CHECK(f.staticBuffersDirect == s.want);
      CHECK(f.inpassClear == appProfileFor(id).inpassClear);   // one field's override leaves the others
    }
    const bool defIn = appProfileFor(id).inpassClear;
    for (const auto& s : sw) {
      AppProfile f = appProfileFor(id);
      CHECK(applyAppProfileSwitch(s.v, f.inpassClear) == s.how);
      CHECK(f.inpassClear == (s.how == Used ? s.want : defIn));
      CHECK(f.staticBuffersDirect == appProfileFor(id).staticBuffersDirect);
    }
  }

  // The fields' log line.
  AppProfile f = appProfileFor(S);
  CHECK(appProfileFieldsLine(f, Unset)
        == "d9mt: app profile fields: staticBuffersDirect on, inpassClear on");
  applyAppProfileSwitch("0", f.staticBuffersDirect);
  CHECK(appProfileFieldsLine(f, Used)
        == "d9mt: app profile fields: staticBuffersDirect off (D9MT_STATIC_BUFFERS_DIRECT), inpassClear on");
  applyAppProfileSwitch("0", f.inpassClear);
  CHECK(appProfileFieldsLine(f, Used, Used)
        == "d9mt: app profile fields: staticBuffersDirect off (D9MT_STATIC_BUFFERS_DIRECT), inpassClear off (D9MT_INPASS_CLEAR)");
  CHECK(appProfileFieldsLine(appProfileFor(G), Unknown)
        == "d9mt: app profile fields: staticBuffersDirect off, inpassClear off");

  if (g_fail)
    return 1;
  std::printf("ok\n");
  return 0;
}
