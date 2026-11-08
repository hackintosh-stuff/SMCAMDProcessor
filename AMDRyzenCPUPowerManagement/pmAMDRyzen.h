//
//  pmAMDRyzen.h
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 3/27/20.
//

#ifndef pmAMDRyzen_h
#define pmAMDRyzen_h


#include <Headers/mach/mach_types.h>
#include <IOKit/IOLib.h>

#include "Headers/osfmk/i386/pmCPU.h"
#include "Headers/osfmk/i386/cpu_topology.h"

#include "symresolver/kernel_resolver.h"

#include <i386/proc_reg.h>
#include "Headers/pmRyzenSymbolTable.h"

#define MOD_NAME pmARyzen
#define XNU_MAX_CPU 64

// 0.7.6: maximum number of iterations pmRyzen_stop() waits for a CPU to leave the
// idle state. It guarantees the stop path converges (the old code looped forever
// when exit_idle always returned true).
#define PMRYZEN_EXIT_IDLE_MAX_SPINS 1000

#define MSR_PSTATE_CTL 0xC0010062
#define MSR_PSTATE_0 0xC0010064

#define EFF_INTERVAL 0.15
#define PSTATE_LIMIT 1
#define PSTATE_LOW_POWER 2
#define PSTATE_STEPDOWN_THRE 0.25
#define PSTATE_STEPUP_THRE 0.45
#define PSTATE_STEPDOWN_TIME 5
#define PSTATE_STEPDOWN_MP_GAIN 1


extern int cpu_number(void);
extern void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

extern x86_lcpu_t *pmRyzen_cpunum_to_lcpu[XNU_MAX_CPU];

extern uint32_t pmRyzen_num_phys;
extern uint32_t pmRyzen_num_logi;

extern uint64_t pmRyzen_exit_idle_c;
extern uint64_t pmRyzen_exit_idle_ipi_c;
extern uint64_t pmRyzen_exit_idle_false_c;

extern uint32_t pmRyzen_hpcpus;

extern uint32_t pmRyzen_pstatelimit;


// 0.7.6 (T8 / O26): pmRyzen_wrmsr_safe now returns int (1 = write succeeded,
// 0 = failed) so callers can detect a failed write. The return type is int rather
// than bool because C's _Bool and C++'s bool are not the same type across a
// cross-language declaration, so int is the only unambiguous choice under
// extern "C" linkage.
extern int pmRyzen_wrmsr_safe(void *, uint32_t, uint64_t);
extern uint64_t pmRyzen_rdmsr_safe(void *, uint32_t);
extern pmRyzen_symtable_t pmRyzen_symtable;

// 0.7.6 (T8 / O26): the -amdpdbg switch mirrored into the PM side. The C file
// cannot see the provider's ADDPR(debugEnabled), so the provider syncs it in
// start(). Logs on the PM side are emitted only while it is on.
extern int pmRyzen_debug_enabled;


typedef struct pmProcessor{
    x86_lcpu_t *lcpu;

    uint64_t stat_exit_idle;
#ifdef PMRYZEN_IDLE_MWAIT
    char pad1[64*64];
#endif
    uint64_t arm_flag;
#ifdef PMRYZEN_IDLE_MWAIT
    char pad2[64*64];
#endif
    uint64_t cpu_awake;
    
    uint64_t last_idle_tsc;
    uint64_t last_start_tsc;
    uint64_t last_idle_length;
    uint64_t last_running_time;
    
    uint64_t eff_timeacc;
    uint64_t eff_idleacc;
    
    float eff_load;
    uint64_t eff_timeaccd;
    uint64_t eff_idleaccd;
    
    uint32_t ll_count;
    uint8_t PState;
    
} pmProcessor_t;

// 0.7.6: a false return means PM registration is unavailable (missing symbols and
// so on) - the caller must fail start() rather than publish a service with 0
// cores (user space would divide logical by physical and crash the kernel).
boolean_t pmRyzen_init(void*);
void pmRyzen_stop(void);
void pmRyzen_PState_reset(void);
float pmRyzen_avgload_pcpu(uint32_t);

uint64_t pmRyzen_machine_idle(uint64_t);

boolean_t pmRyzen_exit_idle(x86_lcpu_t *);

int pmRyzen_choose_cpu(int,int,int);

pmProcessor_t* pmRyzen_get_processor(uint32_t);

// 0.7.6: these three helpers are called inside the all-core barrier (the init
// block and the regular tick of AMDRyzenCPUPowerManagement.cpp). A single
// out-of-bounds read or null dereference inside the barrier is a machine-wide
// deadlock (not even an NMI gets through). They therefore do the full bounds and
// null checks and return conservative values on failure:
//   - phys_num fails -> 0 (that core's frequency data lands in slot 0, benign)
//   - primary_in_core fails -> false (that core is skipped for sampling, benign)
//   - is_master fails -> false (the master does not refresh the P-state table, a
//     functional downgrade but no crash)
inline x86_lcpu_t *pmRyzen_get_lcpu(uint32_t cpunum){
    if(cpunum >= XNU_MAX_CPU) return NULL;
    return pmRyzen_cpunum_to_lcpu[cpunum];
}

inline uint32_t pmRyzen_cpu_phys_num(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL || lcpu->core == NULL) return 0;
    return lcpu->core->pcore_num;
}

inline boolean_t pmRyzen_cpu_primary_in_core(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL || lcpu->core == NULL) return false;
    return lcpu->core->lcpus == lcpu;
}

inline boolean_t pmRyzen_cpu_is_master(uint32_t cpunum){
    x86_lcpu_t *lcpu = pmRyzen_get_lcpu(cpunum);
    if(lcpu == NULL) return false;
    return lcpu->master;
}


#endif /* pmAMDRyzen_h */
