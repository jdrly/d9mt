#include "mtl_memory_profile.h"

#include <windows.h>

#include <cstdlib>

#include "../util/config/config.h"
#include "../util/log/log.h"
#include "../util/util_env.h"

namespace dxvk {

  uint64_t queryPhysicalRam() {
    MEMORYSTATUSEX status = { };
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? uint64_t(status.ullTotalPhys) : 0u;
  }


  MtlMemoryProfile resolveMemoryProfile(const Config& config) {
    std::string name = env::getEnvVar("D9MT_MEMORY_PROFILE");
    if (name.empty())
      name = config.getOption<std::string>("d9mt.memoryProfile", "auto");

    const uint64_t physRam = queryPhysicalRam();

    MtlMemoryTier tier = MtlMemoryTier::Unlimited;
    bool isAuto = false;
    if (!parseMemoryTier(name, tier, isAuto)) {
      Logger::warn(str::format("D9MT: unknown memory profile '", name, "', using auto"));
      isAuto = true;
    }
    if (isAuto)
      tier = autoMemoryTier(physRam);

    MtlMemoryProfile p = memoryProfileForTier(tier);
    p.autoTier = isAuto;
    p.physRam  = physRam;

    if (int32_t mb = config.getOption<int32_t>("d9mt.reportedVramMB", 0); mb > 0)
      p.reportedVramMB = uint32_t(mb);
    if (int32_t mb = config.getOption<int32_t>("d9mt.memoryBudgetMB", 0); mb > 0)
      p.budget = uint64_t(mb) << 20;

    p.writeOnlyDirect = config.getOption<bool>("d9mt.writeOnlyDirect", p.writeOnlyDirect);
    if (int v = parseSwitch(env::getEnvVar("D9MT_WRITEONLY_DIRECT")); v >= 0)
      p.writeOnlyDirect = v != 0;

    if (int32_t n = config.getOption<int32_t>("d9mt.writeOnlyDemoteLocks", -1); n >= 0)
      p.writeOnlyDemoteLocks = uint32_t(n);
    if (std::string v = env::getEnvVar("D9MT_WRITEONLY_DEMOTE"); !v.empty())
      p.writeOnlyDemoteLocks = parseDemoteLocks(v, p.writeOnlyDemoteLocks);

    // D9MT_MEMSTATS=1/true: every 256 frames; =N (>= 16): every N frames; 0/false: off.
    std::string stats = env::getEnvVar("D9MT_MEMSTATS");
    uint32_t every = uint32_t(std::strtoul(stats.c_str(), nullptr, 10));
    if (every)
      p.statsInterval = every >= 16u ? every : 256u;
    else if (int v = parseSwitch(stats); v >= 0 ? v : config.getOption<bool>("d9mt.memoryStats", false))
      p.statsInterval = 256u;

    int labels = parseSwitch(env::getEnvVar("D9MT_LABELS"));
    p.labels = labels >= 0 ? labels != 0 : config.getOption<bool>("d9mt.resourceLabels", false);

    p.pipelineWaitBudgetMs = parseWaitBudgetMs(env::getEnvVar("D9MT_PSO_WAIT_MS"),
      clampWaitBudgetMs(config.getOption<int32_t>("d9mt.pipelineWaitBudgetMs", -1)));

    p.attachmentOpt = config.getOption<bool>("d9mt.attachmentOpt", true);
    if (parseSwitch(env::getEnvVar("D9MT_NO_ATTACHMENT_OPT")) == 1)
      p.attachmentOpt = false;
    return p;
  }

}
