//
//  pmAMDRyzen.c
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 3/27/20.
//

#include "pmAMDRyzen.h"



x86_lcpu_t *pmRyzen_cpunum_to_lcpu[XNU_MAX_CPU];

void *pmRyzen_io_service_handle;

pmProcessor_t pmRyzen_cpus[XNU_MAX_CPU];
uint64_t pmRyzen_tsc_freq;
uint64_t pmRyzen_effective_timetsc;

uint32_t pmRyzen_num_phys;
uint32_t pmRyzen_num_logi;

uint64_t pmRyzen_exit_idle_c;
uint64_t pmRyzen_exit_idle_ipi_c;
uint64_t pmRyzen_exit_idle_false_c;

uint64_t pmRyzen_p_sdtsc;
uint64_t pmRyzen_p_sutsc;

uint32_t pmRyzen_hpcpus = 0;
uint32_t pmRyzen_pstatelimit;

void(*pmRyzen_pmUnRegister)(pmDispatch_t*) = 0;
void(*pmRyzen_cpu_NMI)(int) = 0;
void(*pmRyzen_NMI_enabled)(boolean_t) = 0;
void(*pmRyzen_cpu_IPI)(int) = 0;

// 0.7.6 (T8 / O26): the -amdpdbg switch mirrored into the PM side. The provider
// syncs it in start(); logs on the PM side (a C file) are emitted only while it
// is on.
int pmRyzen_debug_enabled = 0;


//kern_return_t       (*pmCPUStateInit)(void);
//void                (*cstateInit)(void);
//uint64_t            (*MachineIdle)(uint64_t maxIdleDuration);
//uint64_t            (*GetDeadline)(x86_lcpu_t *lcpu);
//uint64_t            (*SetDeadline)(x86_lcpu_t *lcpu, uint64_t);
//void                (*Deadline)(x86_lcpu_t *lcpu);
//boolean_t           (*exitIdle)(x86_lcpu_t *lcpu);
//void                (*markCPURunning)(x86_lcpu_t *lcpu);
//int                 (*pmCPUControl)(uint32_t cmd, void *datap);
//void                (*pmCPUHalt)(void);
//uint64_t            (*getMaxSnoop)(void);
//void                (*setMaxBusDelay)(uint64_t time);
//uint64_t            (*getMaxBusDelay)(void);
//void                (*setMaxIntDelay)(uint64_t time);
//uint64_t            (*getMaxIntDelay)(void);
//void                (*pmCPUSafeMode)(x86_lcpu_t *lcpu, uint32_t flags);
//void                (*pmTimerStateSave)(void);
//void                (*pmTimerStateRestore)(void);
//kern_return_t       (*exitHalt)(x86_lcpu_t *lcpu);
//kern_return_t       (*exitHaltToOff)(x86_lcpu_t *lcpu);
//void                (*markAllCPUsOff)(void);
//void                (*pmSetRunCount)(uint32_t count);
//boolean_t           (*pmIsCPUUnAvailable)(x86_lcpu_t *lcpu);
//int                 (*pmChooseCPU)(int startCPU, int endCPU, int preferredCPU);
//int                 (*pmIPIHandler)(void *state);
//void                (*pmThreadTellUrgency)(int urgency, uint64_t rt_period, uint64_t rt_deadline);
//void                (*pmActiveRTThreads)(boolean_t active);
//boolean_t           (*pmInterruptPrewakeApplicable)(void);
//void                (*pmThreadGoingOffCore)(thread_t old_thread, boolean_t transfer_load,
//                                              uint64_t last_dispatch, boolean_t thread_runnable);

pmDispatch_t pmRyzen_cpuFuncs = {
    .pmCPUStateInit = 0,
    .cstateInit = 0,
    .MachineIdle = &pmRyzen_machine_idle,
    .GetDeadline = 0,
    .SetDeadline = 0,
    .Deadline = 0,
#ifdef PMRYZEN_IDLE_MWAIT
    .exitIdle = &pmRyzen_exit_idle,
#else
    .exitIdle = 0,
#endif
    .markCPURunning = 0,
    .pmCPUControl = 0,
    .pmCPUHalt = 0,
    .getMaxSnoop = 0,
    .setMaxBusDelay = 0,
    .getMaxBusDelay = 0,
    .setMaxIntDelay = 0,
    .getMaxIntDelay = 0,
    .pmCPUSafeMode = 0,
    .pmTimerStateSave = 0,
    .pmTimerStateRestore = 0,
    .exitHalt = 0,
    .exitHaltToOff = 0,
    .markAllCPUsOff = 0,
    .pmSetRunCount = 0,
    .pmIsCPUUnAvailable = 0,
    .pmChooseCPU = &pmRyzen_choose_cpu,
    .pmIPIHandler = 0,
    .pmThreadTellUrgency = 0,
    .pmActiveRTThreads = 0,
    .pmInterruptPrewakeApplicable = 0,
    .pmThreadGoingOffCore = 0,
};

void pmRyzen_init_PState(){
    uint64_t p0 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0);
    
    // 0.7.6 (Round 3 M3): the old expression `(p0 & 0xff) / ((p0 >> 8) & 0x1f)` was an
    // *integer* division, and a zero denominator raises #DE => kernel panic on the
    // start path. A zero denominator has two sources: a failed pmRyzen_rdmsr_safe()
    // returning 0, or the DfsId field itself being 0. Give up on rewriting the
    // firmware when no valid value could be read (degrade, do not crash).
    //
    // 0.7.6 (T8 / O13): DfsId is bits 13:8 (*6 bits*), so the mask changes from 0x1f
    // to 0x3f to match the rest of the tree (dumpPstate() on the cpp side). The old
    // 0x1f truncated bit 13, so for DfsId >= 0x20 the derived P0 frequency (and hence
    // the P1 FID) was wrong.
    uint64_t p0fid = p0 & 0xff;
    uint64_t p0dfs = (p0 >> 8) & 0x3f;
    if(p0dfs == 0 || p0fid == 0) return;
    
    // 0.7.6 (Round 3 M3): switched to floating point division - the integer division
    // truncated the fraction, so the derived P0 frequency (and hence the P1 FID) was
    // wrong. The formula matches dumpPstate() on the cpp side:
    // clock = Fid / DfsId * 200.
    float p0spd = (float)p0fid / (float)p0dfs * 200.0F;
    
    uint64_t p1 = pmRyzen_rdmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0 + 1);
    uint64_t p1dfs = (p1 >> 8) & 0x3f;
    // 0.7.6 (Round 3 M3): a P1 DfsId of 0 would derive p1fid = 0 and write back a P1
    // frequency of 0 (worse than not rewriting at all). Give up as well.
    if(p1dfs == 0) return;
    
    uint64_t p1fid = (uint64_t)((p0spd * 0.80F) / 200.0F * (float)p1dfs);
    // 0.7.6 (Round 3 M3): range guard - FID is only 8 bits wide.
    if(p1fid > 0xffULL) p1fid = 0xffULL;
    // 0.7.6 (Round 4 M4): FID == 0 guard - writing it would make the P1 frequency
    // 0/DfsId*200 = 0 MHz (worse than not rewriting at all). After O13 widened the
    // DfsId mask to 6 bits, p0dfs grows and p1fid shrinks, so the odds of hitting
    // this went up slightly (constructible case:
    // p0fid=64, p0dfs=63, p1dfs=1 => trunc(0.8*64*1/63)=0).
    if(p1fid == 0) return;
    
    // bit63 = PstateEn (1 == enabled) - enables the firmware P-state table entry
    uint64_t p1new = (p1 & ~0xFFULL) | p1fid | (1ULL << 63);
    
    // 0.7.6 (T8 / O26): this used to rewrite the firmware P1 definition with a raw
    // wrmsr64 - a failed raw write could not be detected, so the P1 definition could
    // be corrupted without anyone noticing. It now goes through pmRyzen_wrmsr_safe
    // (the provider's write_msr, which can fail and can be checked).
    // Decision Q2 (a): keep the rewrite, but add guards.
    //
    // 0.7.6 (Round 4 M2): "can be checked" is *conditional* - when
    // _wrmsr_carefully is missing, write_msr() falls back to a raw wrmsr64 that always
    // reports success, and the failure branch below never triggers. The write itself
    // happens on both paths.
    if(!pmRyzen_wrmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_0 + 1, p1new)){
        IOLog("AMDCPUSupport::pmRyzen_init_PState WARN: failed to write MSR 0x%X, "
              "P1 definition left unchanged\n", MSR_PSTATE_0 + 1);
        return;
    }
    
    // 0.7.6 (T8 / O26): print the before/after values under -amdpdbg - rewriting the
    // firmware definition at boot is an "invisible" action, so a problem can only be
    // traced from the logs.
    if(pmRyzen_debug_enabled){
        IOLog("AMDCPUSupport::pmRyzen_init_PState: P0 fid=%llu dfs=%llu (%.1f MHz); "
              "P1 def 0x%llX -> 0x%llX (fid %llu -> %llu)\n",
              p0fid, p0dfs, (double)p0spd,
              (unsigned long long)p1, (unsigned long long)p1new,
              (unsigned long long)(p1 & 0xffULL), (unsigned long long)p1fid);
    }
}

inline void set_PState(pmProcessor_t *cpu, uint8_t state){
    if(pmRyzen_pstatelimit == 0) return;
    state = min(pmRyzen_pstatelimit, state);
    if(cpu->PState == state) return;
    
    boolean_t from_hpstate = !cpu->PState;
    
    pmRyzen_wrmsr_safe(pmRyzen_io_service_handle, MSR_PSTATE_CTL, state);
    cpu->PState = state;
    
    if(!state){
        __asm__ volatile("lock incq (%0)"::"r"(&pmRyzen_hpcpus):"memory");
    } else if(from_hpstate) {
        __asm__ volatile("lock decq (%0)"::"r"(&pmRyzen_hpcpus):"memory");
    }
}

void pmRyzen_doPState_reset(){
    uint32_t cn = cpu_number();
    // 0.7.6 (Round 3 M2): pmRyzen_cpus is a fixed array of XNU_MAX_CPU (64) entries.
    // When the number of logical cores exceeds the array length, cpu_number() is out
    // of range => out-of-bounds write corrupting neighbouring memory. Such cores are
    // not registered during the topology walk either (skipped on the same
    // XNU_MAX_CPU bound), so just skip them here.
    if(cn >= XNU_MAX_CPU) return;
    pmProcessor_t *self = &pmRyzen_cpus[cn];
    self->PState = 8;
    set_PState(self, 0);
}

void pmRyzen_PState_reset(){
    pmRyzen_hpcpus = 0;
    if(pmRyzen_pstatelimit == 0) pmRyzen_pstatelimit = 1;
    mp_rendezvous_no_intrs(&pmRyzen_doPState_reset, NULL);
}

boolean_t pmRyzen_init(void *handle){
    
    pmRyzen_io_service_handle = handle;
    
    // 0.7.6: never dereference a missing symbol.
    //
    // Without _pmDispatch / _pmUnRegister pmDispatch cannot be registered and the PM
    // capability of this kext is entirely unavailable => initialization fails (the
    // caller turns that into start() returning false, so the service is not
    // published) instead of a null pointer dereference crashing the kernel.
    //
    // A missing _tscFreq makes the time base used by the idle callback (eff_timeacc
    // and the thresholds) meaningless. That is handled by *graceful degradation*, not
    // by failing initialization: see the _tscFreq branch below.
    //
    // A missing _i386_cpu_IPI only affects the stop path (sending an IPI to wake an
    // idle CPU), so a null check at the call site is enough; it must not keep the
    // whole service from starting.
    if(!pmRyzen_symtable._pmDispatch || !pmRyzen_symtable._pmUnRegister){
        IOLog("AMDCPUSupport::pmRyzen_init WARN: _pmDispatch/_pmUnRegister missing, PM registration impossible\n");
        pmRyzen_num_phys = 0;
        pmRyzen_num_logi = 0;
        return false;
    }
    
    // A missing _tscFreq makes the time base used by the idle callback (eff_timeacc
    // and the thresholds) meaningless. Instead of failing to start, degrade:
    // pmRyzen_tsc_freq stays 0 and pmRyzen_machine_idle() skips the load-based
    // P-state scaling (otherwise every threshold is 0 and the CPU gets pinned in P0,
    // wasting power and producing heat). Package power measurement is degraded
    // separately by the caller (AMDRyzenCPUPowerManagement.cpp).
    if(!pmRyzen_symtable._tscFreq){
        IOLog("AMDCPUSupport::pmRyzen_init WARN: _tscFreq missing, "
              "idle-based P-state scaling disabled\n");
        pmRyzen_tsc_freq = 0;
    }
    
    void **kernelDisp = pmRyzen_symtable._pmDispatch;
    pmRyzen_pmUnRegister = (void(*)(pmDispatch_t*))pmRyzen_symtable._pmUnRegister;
    pmRyzen_cpu_NMI = (void(*)(int))pmRyzen_symtable._cpu_NMI_interrupt;
    pmRyzen_NMI_enabled = (void(*)(boolean_t))pmRyzen_symtable._NMIPI_enable;
    pmRyzen_cpu_IPI = (void(*)(boolean_t))pmRyzen_symtable._i386_cpu_IPI;
    if(pmRyzen_symtable._tscFreq){
        pmRyzen_tsc_freq = *((uint64_t*)pmRyzen_symtable._tscFreq);
    }
    
    
    
    pmCallBacks_t cb;
    if(*kernelDisp)(*pmRyzen_pmUnRegister)(*kernelDisp);
    pmKextRegister(PM_DISPATCH_VERSION, &pmRyzen_cpuFuncs, &cb);
//    pmKextRegister(PM_DISPATCH_VERSION, NULL, &cb);
    
    x86_pkg_t * pkg = cb.GetPkgRoot();
    int pkgCount = 0;
    
    pmRyzen_num_phys = 0;
    pmRyzen_num_logi = 0;
    pmRyzen_hpcpus = 0;
    // enables low power state by default
    pmRyzen_pstatelimit = PSTATE_LOW_POWER;
    
    while(pkg){
        pkgCount++;
        x86_core_t *core = pkg->cores;
        
        while (core) {
            x86_lcpu_t *lcpu = core->lcpus;
            // 0.7.6 (Round 2 F2): count only physical cores with at least one successfully
            // registered lcpu. It used to increment pmRyzen_num_phys unconditionally, which on
            // a machine with more than 64 logical cores could make num_phys > num_logi (or
            // even > 64) - user space uses it as the loop bound for effFreq_perCore, so it
            // would read out of bounds and copy the neighbouring memory to user space.
            int coreRegistered = 0;
            
            while (lcpu) {
                // 0.7.6: bounds check. cpu_num comes from the kernel topology and should not
                // exceed XNU_MAX_CPU, but pmRyzen_cpunum_to_lcpu / pmRyzen_cpus are fixed arrays
                // and an out-of-bounds write corrupts memory. These helpers are called inside the
                // all-core barrier, so the consequence would be a machine-wide deadlock.
                if(lcpu->cpu_num >= XNU_MAX_CPU){
                    IOLog("AMDCPUSupport::pmRyzen_init WARN: lcpu cpu_num %u out of range (max %u), skipped\n",
                          lcpu->cpu_num, (unsigned int)(XNU_MAX_CPU - 1));
                    lcpu = lcpu->next_in_core;
                    continue;
                }
                
                pmRyzen_num_logi++;
                coreRegistered = 1;
//                IOLog("LCPU: %u master:%u, primary:%u\n", lcpu->cpu_num, lcpu->master, lcpu->primary);
                
                pmRyzen_cpunum_to_lcpu[lcpu->cpu_num] = lcpu;
                pmProcessor_t *cpu = &pmRyzen_cpus[lcpu->cpu_num];
                cpu->lcpu = lcpu;
                cpu->stat_exit_idle = 0;
                cpu->arm_flag = 0;
                cpu->cpu_awake = 1;
                
                lcpu = lcpu->next_in_core;
            }
            
            if(coreRegistered) pmRyzen_num_phys++;
            
            core = core->next_in_pkg;
        }
        
        pkg = pkg->next;
    }
//    IOLog("pkg c %d\n", pkgCount);

    
    pmRyzen_effective_timetsc = ((double)pmRyzen_tsc_freq * EFF_INTERVAL);
    pmRyzen_p_sdtsc = (uint64_t)((double)pmRyzen_effective_timetsc * PSTATE_STEPDOWN_THRE);
    pmRyzen_p_sutsc = (uint64_t)((double)pmRyzen_effective_timetsc * PSTATE_STEPUP_THRE);
    
    pmRyzen_init_PState();
    pmRyzen_PState_reset();
    
    cb.initComplete();
    
    return true;
}

void pmRyzen_stop(){
    
    if(pmRyzen_pmUnRegister){
        (*pmRyzen_pmUnRegister)(&pmRyzen_cpuFuncs);
    }
    
    //Make sure all cores exited idle thread.
    //
    // 0.7.6: this loop used to be able to spin forever. In a non-MWAIT build
    // pmRyzen_exit_idle() always returned true, so while(exit_idle(...)) never
    // exited - an endless stream of IPIs on the kext stop path, and the machine stops
    // responding. exit_idle() now reports whether the CPU actually woke up, and the
    // iteration cap below guarantees convergence in any case.
    for (int i = 0; i < pmRyzen_num_logi; i++) {
        if(i >= XNU_MAX_CPU) break;
        x86_lcpu_t *lcpu = pmRyzen_cpunum_to_lcpu[i];
        if(lcpu == NULL) continue;
        
        int spins = 0;
        while(pmRyzen_exit_idle(lcpu)){
            if(pmRyzen_cpu_IPI){
                (*pmRyzen_cpu_IPI)(i);
            }
            if(++spins >= PMRYZEN_EXIT_IDLE_MAX_SPINS){
                IOLog("AMDCPUSupport::pmRyzen_stop: CPU %d did not leave idle after %d attempts, giving up\n",
                      i, spins);
                break;
            }
        }
    }
}



float pmRyzen_avgload_pcpu(uint32_t cpu){
    float loadacc = 0;
    int num_lcpus = 0;
    
    // 0.7.6 (Round 2 F3): this used to dereference pmRyzen_cpunum_to_lcpu[cpu]->core
    // directly, and this function is reachable from user space through selector 6
    // (the index is derived from the logical/physical counts). If the topology has
    // holes in cpu_num, hitting one is a NULL dereference - a single user-space call
    // crashes the kernel. Use the helper with the bounds and NULL checks instead.
    x86_lcpu_t *entry = pmRyzen_get_lcpu(cpu);
    if(entry == NULL || entry->core == NULL) return 0.0f;
    
    x86_lcpu_t *lcpu = entry->core->lcpus;
    while (lcpu) {
        if(lcpu->cpu_num < XNU_MAX_CPU){
            float idleAccd = (float)pmRyzen_cpus[lcpu->cpu_num].eff_idleaccd;
            float timeAccd = (float)pmRyzen_cpus[lcpu->cpu_num].eff_timeaccd;
            // 0.7.6: timeAccd can be 0 (the sampling window has not elapsed yet) and the
            // division would produce inf/NaN.
            if(timeAccd > 0)
                loadacc += 1 - idleAccd / timeAccd;
        }
        num_lcpus++;
        lcpu = lcpu->next_in_core;
    }
    
    return num_lcpus > 0 ? loadacc / (float)num_lcpus : 0.0f;
}


uint32_t pmRyzen_last_woken_cpu=0;
//uint32_t pmRyzen_last_idle_cpu=0;

// 0.7.6 (Round 3 M2): pmRyzen_cpus[] is a fixed array of XNU_MAX_CPU (64) entries.
// When the number of logical cores exceeds 64, cpu_number() is out of range. Such
// cores are not registered during the topology walk (there is no point accounting
// for them), but XNU still calls the idle callback for them - returning early
// would leave those cores spinning forever. Degraded fallback: point the state at
// this static scratch structure, so the accounting lands there (meaningless but
// harmless) and the idle wait still happens.
static pmProcessor_t pmRyzen_untracked_cpu;

uint64_t pmRyzen_machine_idle(uint64_t maxDur){

    uint32_t cn = cpu_number();
    bool tracked = (cn < XNU_MAX_CPU);
    pmProcessor_t *self = tracked ? &pmRyzen_cpus[cn] : &pmRyzen_untracked_cpu;
    
//    pmRyzen_last_idle_cpu = cn;
    
    __asm__ volatile("cli;");
    
    self->cpu_awake = 0;
    self->arm_flag = 0;
    
    uint64_t tscnow = rdtsc64();

    self->last_idle_tsc = tscnow;
//    self->last_running_time = self->last_idle_tsc - self->last_start_tsc;
    
#ifdef PMRYZEN_IDLE_MWAIT

    void* addr = &self->arm_flag;
    uint32_t ps_hint = 0x50;
    __asm__ volatile("wbinvd":::"memory");
    __asm__ volatile("mfence":::"memory");
    __asm__ volatile("clflushopt %0" : "+m" (*(volatile char *)&self->arm_flag));

    __asm__ volatile("mfence;"
                     "movq %0, %%rax;"
                     "xor %%edx, %%edx;"
                     "xor %%ecx, %%ecx;"
                     "monitor;"
                     "xorq %%rax, %%rax;"
                     "movl %1, %%eax;"
                     "movl $0x1, %%ecx;"
                     "mwait;"
                     :
                     : "r"(addr), "r"(ps_hint)
                     : "%ecx", "%edx", "%rax"
                     );
    
#elif PMRYZEN_IDLE_SIMPLE
    
    __asm__ volatile("sti;hlt;");
    
#elif PMRYZEN_IDLE_IO_CSTATE
    
    __asm__ volatile("sti;"
                     "inw $0xf2, %%ax;"
                     "cli;"
                     :::"%eax");
    
#endif

    
    self->cpu_awake = 1;
    if(!self->arm_flag)
        pmRyzen_exit_idle_false_c++;
    
    
    tscnow = rdtsc64();

    uint64_t tscela = tscnow - self->last_idle_tsc;
    self->eff_timeacc += tscnow - self->last_start_tsc;
    self->eff_idleacc += tscela;

    // 0.7.6: pmRyzen_tsc_freq == 0 means the _tscFreq symbol is missing and the time
    // base is unavailable. pmRyzen_effective_timetsc / p_sutsc / p_sdtsc are all 0
    // then, so entering this branch would always take the "step up" path (rt > 0)
    // and pin the CPU in P0. Skip the load-based P-state scaling entirely in that
    // case (degrade, do not crash).
    // 0.7.6 (Round 3 M2): unregistered cores (tracked == false) do not take part in
    // the scaling - they share the scratch structure, so their load history is
    // meaningless.
    if(tracked && pmRyzen_tsc_freq > 0 && self->eff_timeacc > pmRyzen_effective_timetsc){
//        self->eff_load = 1 - (float)self->eff_idleacc / (float)self->eff_timeacc;
        uint64_t rt = self->eff_timeacc - self->eff_idleacc;
        
        //Avoid using xmm registers shared within same core.
        if(rt > pmRyzen_p_sutsc){
            if (self->PState > 0) {
                set_PState(self, self->PState-1);
            } else {
                set_PState(self, 0);
            }
            self->ll_count = 0;
        } else if(rt < pmRyzen_p_sdtsc){
            self->ll_count++;
            if(self->ll_count > PSTATE_STEPDOWN_TIME + pmRyzen_hpcpus * PSTATE_STEPDOWN_MP_GAIN){
                self->ll_count = 0;
                set_PState(self, self->PState+1);
            }
        }
        
//        self->eff_load = 1 - (float)self->eff_idleacc / (float)self->eff_timeacc;
        self->eff_idleaccd = self->eff_idleacc;
        self->eff_timeaccd = self->eff_timeacc;
        self->eff_timeacc = 0;
        self->eff_idleacc = 0;

        
//        if(self->eff_load > PSTATE_STEPUP_THRE){
//            set_PState(self, 0);
//            self->ll_count = 0;
//        } else if(self->eff_load < PSTATE_STEPDOWN_THRE){
//            self->ll_count++;
//            if(self->ll_count > PSTATE_STEPDOWN_TIME){
//                set_PState(self, self->PState+1);
//            }
//        }

    }

    self->last_start_tsc = tscnow;
    self->last_idle_length = tscela;
    
    // 0.7.6 (Round 3 M2): record only registered cores, which keeps
    // pmRyzen_last_woken_cpu < XNU_MAX_CPU (pmRyzen_choose_cpu() uses it to index
    // pmRyzen_cpus[]).
    if(tracked) pmRyzen_last_woken_cpu = cn;
    return 0;
}

boolean_t pmRyzen_exit_idle(x86_lcpu_t *lcpu){
    
    // 0.7.6: bounds check (pmRyzen_cpus is a fixed array)
    if(lcpu == NULL || lcpu->cpu_num >= XNU_MAX_CPU) return false;
    
    pmProcessor_t *target = &pmRyzen_cpus[lcpu->cpu_num];

    // Exit if cpu is already awake.
//    if(target->cpu_awake) return false;
    
//    pmRyzen_exit_idle_c++;
    
#ifdef PMRYZEN_IDLE_MWAIT
    uint64_t start_tsc = rdtsc64();
    do {
        target->arm_flag = 1;
        __asm__ volatile("pause;");
//        asm volatile("clflushopt %0" : "+m" (*(volatile char *)&target->arm_flag));
//        __asm__ volatile("mfence;");

        if(rdtsc64() - start_tsc > 0x6000){
            //If we still unable to wake up the processor, send an IPI.
            pmRyzen_exit_idle_ipi_c++;
            return true;
        }
    } while(!target->cpu_awake);
    
    return false;
    
#else
    target->arm_flag = 1;
//    pmRyzen_exit_idle_ipi_c++;
    
    // 0.7.6: a non-MWAIT build has no monitor/mwait, so a CPU can only leave the idle
    // state on a pending interrupt. This used to return true unconditionally -
    // pmRyzen_stop()'s while(exit_idle(...)) therefore looped forever (sending one
    // more IPI per iteration) and hung the machine on the kext stop path. It now
    // reports whether the CPU actually woke up: still idle means true again.
    if(target->cpu_awake){
        pmRyzen_exit_idle_false_c++;
        return false;
    }
    
    return true;
#endif
}

int pmRyzen_choose_cpu(int startCPU, int endCPU, int preferredCPU){
    
    //We only provide a hint as scheduler will make the final decision anyway.
  
//    return preferredCPU;
    
    // 0.7.6 (Round 3 M2): preferredCPU is passed in by XNU and can be outside
    // pmRyzen_cpus[] (more than 64 logical cores) => out-of-bounds read.
    // pmRyzen_last_woken_cpu has the same problem.
    if(preferredCPU >= 0 && preferredCPU < XNU_MAX_CPU && pmRyzen_cpus[preferredCPU].cpu_awake)
        return preferredCPU;

    if(pmRyzen_last_woken_cpu < XNU_MAX_CPU && pmRyzen_cpus[pmRyzen_last_woken_cpu].cpu_awake)
        return pmRyzen_last_woken_cpu;
    
//    for(int i = startCPU; i < endCPU; i++){
//        if(i != pmRyzen_last_idle_cpu && pmRyzen_cpus[i].cpu_awake)
//            return i;
//    }
    
    return preferredCPU;
}

pmProcessor_t* pmRyzen_get_processor(uint32_t cpu){
    // 0.7.6 (Round 3 M2): bounds check (pmRyzen_cpus is a fixed array)
    if(cpu >= XNU_MAX_CPU) return NULL;
    return &pmRyzen_cpus[cpu];
}
