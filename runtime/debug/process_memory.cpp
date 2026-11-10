#include "process_memory.h"

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <psapi.h>
#   pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#   include <mach/mach.h>
#endif

namespace ProcessMemory
{
    Info Get()
    {
        Info info;
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        {
            info.footprint = pmc.PrivateUsage;
            info.footprintPeak = pmc.PeakPagefileUsage;
            info.resident = pmc.WorkingSetSize;
            info.residentPeak = pmc.PeakWorkingSetSize;
        }
#elif defined(__APPLE__)
        task_vm_info_data_t vm{};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS)
        {
            info.footprint = vm.phys_footprint;
            info.footprintPeak = vm.ledger_phys_footprint_peak;
            info.resident = vm.resident_size;
            info.residentPeak = vm.resident_size_peak;
        }
#endif
        return info;
    }
}
