#ifndef UNCORE_H
#define UNCORE_H

#include "champsim.h"
#include "cache.h"
#include "dram_controller.h"
#include "dram_cache_manager.h"
//#include "drc_controller.h"

//#define DRC_MSHR_SIZE 48

// uncore
class UNCORE {
  public:

    // LLC
    CACHE LLC{"LLC", LLC_SET, LLC_WAY, LLC_SET*LLC_WAY, LLC_WQ_SIZE, LLC_RQ_SIZE, LLC_PQ_SIZE, LLC_MSHR_SIZE};

    // DRAM cache device (near/local memory backing the DRAM_CACHE_MANAGER).
    // Reuses the existing MEMORY_CONTROLLER/DRAM timing model unmodified
    // (see docs/gem5_to_champsim_mapping.md).
    MEMORY_CONTROLLER DRAM_CACHE_DEVICE{"DRAM_CACHE"};

    // Far/backing memory. Same object that used to sit directly below the
    // LLC; it now sits below the DRAM_CACHE_MANAGER instead.
    MEMORY_CONTROLLER DRAM{"DRAM"};

    // DRAM Cache Manager: replaces uncore.DRAM as the LLC's lower_level.
    // Owns no memory itself; forwards to DRAM_CACHE_DEVICE (near) and
    // DRAM (far). See docs/gem5_to_champsim_mapping.md and
    // docs/feature_coverage.md for what it currently does.
    DRAM_CACHE_MANAGER DCM{"DCM", &DRAM_CACHE_DEVICE, &DRAM};

    UNCORE();
};

extern UNCORE uncore;

#endif
