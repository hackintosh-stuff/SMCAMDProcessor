#include "AMDRyzenCPUPowerManagement.hpp"
#include <string.h>

OSDefineMetaClassAndStructors(AMDRyzenCPUPowerManagement, IOService);

#define TCTL_OFFSET_TABLE_LEN 6
static constexpr const struct tctl_offset tctl_offset_table[] = {
    { 0x17, "AMD Ryzen 5 1600X", 20 },
    { 0x17, "AMD Ryzen 7 1700X", 20 },
    { 0x17, "AMD Ryzen 7 1800X", 20 },
    { 0x17, "AMD Ryzen 7 2700X", 10 },
    { 0x17, "AMD Ryzen Threadripper 19", 27 }, /* 19{00,20,50}X */
    { 0x17, "AMD Ryzen Threadripper 29", 27 }, /* 29{20,50,70,90}[W]X */
};

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

// 0.7.6: maximum number of *retries* for a symbol lookup (the first attempt is
// not counted, so 21 lookups happen in total). It used to retry forever: once a
// symbol is gone (a future macOS, a changed symbol table, stripped symbols) the
// kext would hang in init() and never publish its service, which shows up as
// "some kext is missing".
// 0.7.6 (Round 2 F5): tightened from "retry forever (2 ms each)" to a bound.
// 0.7.6 (Round 3 m1): the old comment claimed "1 s per retry => ~20 s bound",
// which did not match the implementation: this constant is in *milliseconds*, so
// the real total wait is ~20 ms. 20 ms is the right budget: _wrmsr_carefully is a
// static kernel symbol, it either exists or it does not and will not show up
// later. Waiting longer only slows boot down (a macOS release that drops this
// private symbol would waste 20 s on every boot).
static constexpr uint32_t kSymbolLookupMaxAttempts = 20;
static constexpr uint32_t kSymbolLookupRetryDelayMs = 1;

// 0.7.6: per-CPU retry budget for the HWCR initialization (reset on every wake).
// Retries on the wake path must be bounded: otherwise a permanently unreadable
// HWCR means one read plus one write inside the no-interrupt barrier on every
// tick, with no end to the exposure window.
static constexpr uint8_t PMRYZEN_HWCR_RETRY_BUDGET = 20;


extern "C"{
// 0.7.6 (T8 / O26): returns 1 on a successful write, 0 on failure.
// The return type is int rather than bool: this header is visible to C, and C's
// _Bool is not the same type as C++'s bool, so int is the only unambiguous
// choice under extern "C" linkage.
int pmRyzen_wrmsr_safe(void *handle, uint32_t addr, uint64_t value){
    return static_cast<AMDRyzenCPUPowerManagement*>(handle)->write_msr(addr, value) ? 1 : 0;
}

uint64_t pmRyzen_rdmsr_safe(void *handle, uint32_t addr){
    uint64_t v = 0;
    static_cast<AMDRyzenCPUPowerManagement*>(handle)->read_msr(addr, &v);
    return v;
}

pmRyzen_symtable_t pmRyzen_symtable={0};
// 0.7.6: pmRyzen_symtable_ready was removed - it was only ever written (the loop
// waiting on it had long been commented out), so keeping it only misleads the
// next reader into thinking a synchronization point still exists.

}


bool AMDRyzenCPUPowerManagement::init(OSDictionary *dictionary){
//    strcpy((char*)kMODULE_VERSION, xStringify(MODULE_VERSION), (uint32_t)strlen(xStringify(MODULE_VERSION)));
    IOLog("AMDCPUSupport v%s, init\n", xStringify(MODULE_VERSION));
    
    IOLog("AMDCPUSupport::enter dlinking..\n");
    
    // 0.7.6: the lookup must be bounded. Once the budget is exhausted we stop
    // waiting: write_msr() has its own raw wrmsr64 fallback (see write_msr), so the
    // service can still be published, it only loses the "safe write" path.
    uint32_t lookupAttempts = 0;
    
retry:
    find_mach_header_addr(getKernelVersion() >= KernelVersion::BigSur);
    pmRyzen_symtable._wrmsr_carefully = lookup_symbol("_wrmsr_carefully");
    
    if(!pmRyzen_symtable._wrmsr_carefully){
        // 0.7.6 (Round 2 F5): kextloadAlerts is incremented only on the first failure.
        // It used to be incremented on every retry, so the alert the user saw became
        // "Kext alert detected: 21" - semantically wrong (that is one alert retried
        // many times, not 21 alerts).
        if(lookupAttempts == 0) kextloadAlerts++;
        if(lookupAttempts++ < kSymbolLookupMaxAttempts){
            IOSleep(kSymbolLookupRetryDelayMs);
            goto retry;
        }
        // 0.7.6 (Round 3 m1): log both the number of attempts actually made and the
        // retry limit. The loop tests before incrementing, so on exit
        // lookupAttempts == limit + 1; printing only one of the two numbers makes the
        // log hard to match against the code.
        IOLog("AMDCPUSupport::init WARN: _wrmsr_carefully not found after %u attempt(s) "
              "(max retries %u), falling back to unsafe wrmsr64\n",
              lookupAttempts, kSymbolLookupMaxAttempts);
    }
    
    
    pmRyzen_symtable._KUNCUserNotificationDisplayAlert = lookup_symbol("_KUNCUserNotificationDisplayAlert");
    pmRyzen_symtable._cpu_to_processor = lookup_symbol("_cpu_to_processor");
    pmRyzen_symtable._tscFreq = lookup_symbol("_tscFreq");
    pmRyzen_symtable._pmDispatch = lookup_symbol("_pmDispatch");
    pmRyzen_symtable._pmUnRegister = lookup_symbol("_pmUnRegister");
    pmRyzen_symtable._cpu_NMI_interrupt = lookup_symbol("_cpu_NMI_interrupt");
    pmRyzen_symtable._NMIPI_enable = lookup_symbol("_NMIPI_enable");
    pmRyzen_symtable._i386_cpu_IPI = lookup_symbol("_i386_cpu_IPI");
    IOLog("AMDCPUSupport::enter link finished.\n");
    return IOService::init(dictionary);
}

void AMDRyzenCPUPowerManagement::free(){
    IOService::free();
}


bool AMDRyzenCPUPowerManagement::getPCIService(){
    
    
    OSDictionary *matching_dict = serviceMatching("IOPCIDevice");
    if(!matching_dict){
        IOLog("AMDCPUSupport::getPCIService: serviceMatching unable to generate matching dictonary.\n");
        return false;
    }
    
    //Wait for PCI services to init.
    waitForMatchingService(matching_dict);
    
    OSIterator *service_iter = getMatchingServices(matching_dict);
    IOPCIDevice *service = 0;
    
    if(!service_iter){
        IOLog("AMDCPUSupport::getPCIService: unable to find a matching IOPCIDevice.\n");
        return false;
    }
    
    while (true){
        OSObject *obj = service_iter->getNextObject();
        if(!obj) break;
        
        service = OSDynamicCast(IOPCIDevice, obj);
        break;
    }
    
    if(!service){
        IOLog("AMDCPUSupport::getPCIService: unable to get IOPCIDevice on host system.\n");
        return false;
    }
    
    IOLog("AMDCPUSupport::getPCIService: succeed!\n");
    fIOPCIDevice = service;
    
    return true;
}

void AMDRyzenCPUPowerManagement::initWorkLoop() {
    IOLog("AMDCPUSupport::startWorkLoop setting up timer");
    
    timerEvent_main = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        AMDRyzenCPUPowerManagement *provider = OSDynamicCast(AMDRyzenCPUPowerManagement, object);

        //Run initialization
        if(!provider->serviceInitialized){
            IOLog("AMDCPUSupport::startWorkLoop initialize service");
            
            //Disable interrupts and sync all processor cores.
            mp_rendezvous_no_intrs([](void *obj) {
                auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
                
                provider->write_msr(kMSR_CSTATE_ADDR, 0xf0);
                
//                uint64_t val = 0;
//                provider->read_msr(0xC0010292, &val);
//                provider->write_msr(0xC0010292, val | ((uint64_t)1 << 32));
//
//                provider->read_msr(0xC0010296, &val);
//                provider->write_msr(0xC0010296, val | (1 << 22) | (1 << 14) | (1 << 6));

                uint32_t cpu_num = cpu_number();

                // 0.7.6 (Round 2 F1): on a machine with more than 64 logical cores
                // cpu_number() can be >= CPUInfo::MaxCpus, while every per-CPU array below is a
                // fixed 64-entry array. Using it as an index is an out-of-bounds write, and it
                // would happen inside the all-core no-interrupt barrier (memory corruption plus a
                // machine-wide deadlock). Skip such cores.
                if(cpu_num >= CPUInfo::MaxCpus) return;

                uint64_t hwConfig;
                // Never panic here: this block re-runs on every wake from sleep.
                // A failed read — or a failed write — is remembered per CPU and
                // retried by the regular timer tick below, so the init is not
                // lost for this wake cycle.
                //
                // 0.7.6: reset this CPU's retry budget on every wake. Retries on the wake path
                // must be bounded, otherwise a permanently failing read means one read plus one
                // write inside the no-interrupt barrier on every tick (50-1200 ms) forever.
                provider->hwcrInitRetryLeft[cpu_num] = PMRYZEN_HWCR_RETRY_BUDGET;
                
                if(provider->read_msr(kMSR_HWCR, &hwConfig)){
                    hwConfig |= (1 << 30);
                    provider->hwcrInitPending[cpu_num] = !provider->write_msr(kMSR_HWCR, hwConfig);
                } else {
                    provider->hwcrInitPending[cpu_num] = true;
                }


                //Read PStateDef generated by EFI.
                if(pmRyzen_cpu_is_master(cpu_num)){
                    provider->pstateDumpRetryLeft = 20;
                    provider->pstateDumpPending = !provider->dumpPstate(nullptr, false);
                }


                if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
                uint32_t physicalRaw = pmRyzen_cpu_phys_num(cpu_num);
                // 0.7.6 (Round 2 F1): the physical core index can exceed the fixed array as
                // well (pcore_num can reach 255 on a machine with more than 64 physical cores).
                if(physicalRaw >= CPUInfo::MaxCpus) return;
                uint8_t physical = (uint8_t)physicalRaw;


                //Init performance frequency counter.
                uint64_t APERF, MPERF;
                if(provider->read_msr(kMSR_APERF, &APERF) && provider->read_msr(kMSR_MPERF, &MPERF)){
                    provider->lastAPERF_perCore[physical] = APERF;
                    provider->lastMPERF_perCore[physical] = MPERF;
                    provider->freqCountersSeeded[physical] = true;
                }

            }, provider);
            
            // 0.7.5: post-wake HWCR snapshot for drift monitoring.
            // Deliberately done OUTSIDE the mp_rendezvous_no_intrs barrier:
            // IOLog takes the console/os_log global lock, and XNU asserts
            // "IOLog called with interrupts disabled" — inside that barrier
            // every CPU runs with interrupts disabled.
            if (ADDPR(debugEnabled)) {
                uint64_t hwcr = 0;
                if (provider->read_msr(kMSR_HWCR, &hwcr)) {
                    IOLog("AMDCPUSupport::init post-wake HWCR=0x%llx CpbDis=%u\n",
                          hwcr, (unsigned int)((hwcr >> 25) & 1U));
                }
                uint32_t pending = 0;
                for (uint32_t i = 0; i < CPUInfo::MaxCpus; i++) {
                    if (provider->hwcrInitPending[i]) pending++;
                }
                if (pending > 0) {
                    IOLog("AMDCPUSupport::init HWCR init pending on %u CPU(s), will retry\n", pending);
                }
            }
            
            //Make all cores P0 state by default.
            provider->PStateCtl = 0;
            
            provider->serviceInitialized = true;
            provider->timerEvent_main->setTimeoutMS(1);
            return;
        }
        
        
        mp_rendezvous_no_intrs([](void *obj) {
            auto provider = static_cast<AMDRyzenCPUPowerManagement*>(obj);
            uint32_t cpu_num = cpu_number();

            // 0.7.6 (Round 2 F1): every per-CPU array is a fixed 64-entry array, and
            // cpu_number() can be out of range on a machine with more than 64 logical cores -
            // skip such cores.
            if(cpu_num >= CPUInfo::MaxCpus) return;

            // 0.7.5: retry the HWCR init on CPUs where the read or write
            // failed before. Every CPU only touches its own slot, so there is
            // no cross-CPU race. The master CPU also retries a failed P-state
            // table dump so a transient read failure is not silently final.
            //
            // 0.7.6: the retry count is now bounded (one budget per CPU, reset on wake).
            // It used to be unbounded: a permanently failing HWCR read meant one read plus
            // one write inside the no-interrupt barrier on every tick, forever.
            if(provider->hwcrInitPending[cpu_num]){
                if(provider->hwcrInitRetryLeft[cpu_num] > 0){
                    provider->hwcrInitRetryLeft[cpu_num]--;
                    uint64_t hwConfig;
                    if(provider->read_msr(kMSR_HWCR, &hwConfig)){
                        hwConfig |= (1 << 30);
                        if(provider->write_msr(kMSR_HWCR, hwConfig))
                            provider->hwcrInitPending[cpu_num] = false;
                    }
                }
            }

            if(provider->pstateDumpPending && pmRyzen_cpu_is_master(cpu_num)){
                // Bounded: every rdmsr takes cpu_lock inside the barrier, so
                // a permanently unreadable P-state MSR must not retry every
                // tick forever. The budget is refilled on wake re-init and
                // on explicit user writePstate().
                if(provider->pstateDumpRetryLeft > 0){
                    provider->pstateDumpRetryLeft--;
                    provider->pstateDumpPending = !provider->dumpPstate(nullptr, false);
                    // 0.7.6: no IOLog inside the barrier - IOLog takes the global log lock, and
                    // every CPU here has interrupts disabled (XNU also asserts "IOLog called with
                    // interrupts disabled"). Only set the flag here; the caller outside the barrier
                    // prints it.
                    if(provider->pstateDumpPending && provider->pstateDumpRetryLeft == 0)
                        provider->pstateDumpRetryExhausted = true;
                }
            }

            provider->updateInstructionDelta(cpu_num);

            // Ignore hyper-threaded cores
            if(!pmRyzen_cpu_primary_in_core(cpu_num)) return;
            uint32_t physicalRaw = pmRyzen_cpu_phys_num(cpu_num);
            // 0.7.6 (Round 2 F1): clamp the physical core index (same as above).
            if(physicalRaw >= CPUInfo::MaxCpus) return;
            uint8_t physical = (uint8_t)physicalRaw;


            provider->calculateEffectiveFrequency(physical);

        }, provider);
        
        // 0.7.6: report point outside the barrier. Inside the barrier every CPU has
        // interrupts disabled and IOLog takes the global log lock (XNU also asserts
        // "IOLog called with interrupts disabled"), so the retry-exhausted message must
        // be printed here rather than inside the barrier.
        if(provider->pstateDumpRetryExhausted){
            provider->pstateDumpRetryExhausted = false;
            if (ADDPR(debugEnabled))
                IOLog("AMDCPUSupport::initWorkLoop: P-state dump retries exhausted, keeping last-good table\n");
        }
        
        //Read stats from package.
        provider->updatePackageTemp();
        provider->updatePackageEnergy();
        

        
        uint32_t now = uint32_t(getCurrentTimeNs() / 1000000); //ms
        uint32_t newInt = max(now - provider->timeOfLastMissedRequest,
                              provider->estimatedRequestTimeInterval);

        provider->actualUpdateTimeInterval = now - provider->timeOfLastUpdate;
        provider->timeOfLastUpdate = now;
        provider->updateTimeInterval = min(1200, max(50, newInt));
        
        provider->timerEvent_main->setTimeoutMS(provider->updateTimeInterval);

//        IOLog("fpp %d %d %.4f.\n", HF_TEMP_SAMPLE_FREQ, HF_TEMP_SAMPLE_PERIOD, (float)HF_TEMP_SAMPLE_REP);
        
    });
    
//    tempSamplePeriod = (int)((1.0f / (float)HF_TEMP_SAMPLE_FREQ) * 1000);
    float fillT = getPackageTemp();
    tempNextSample = 0;
    for (int i = 0; i < HF_TEMP_SAMPLE_LEN; i++) tempSamples[i] = fillT;
    
    timerEvent_tempe = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
        AMDRyzenCPUPowerManagement *provider = OSDynamicCast(AMDRyzenCPUPowerManagement, object);
        
        int next_samp = provider->tempNextSample;
        float t = provider->getPackageTemp();
        provider->tempSamples[next_samp] = t;
        provider->tempNextSample = (next_samp + 1) % HF_TEMP_SAMPLE_LEN;
        
        sender->setTimeoutMS(HF_TEMP_SAMPLE_PERIOD);
    });
    
    registerService();
    
    lastUpdateTime = getCurrentTimeNs();
    pwrLastTSC = rdtsc64();
    // 0.7.6 (T8 / Round 4 M1): seed the energy baseline.
    // lastUpdateEnergyValue was never seeded, so the first sample took the whole RAPL
    // counter (cumulative since boot) against a ~50 ms denominator and reported a
    // multi-kilowatt phantom spike. That spike showed up on the first line of the
    // -amdpdbg dual-value log (polluting the one-week comparison) and was also visible
    // to the app and to VirtualSMC for one sample period.
    // Reuse the mechanism introduced by O24: the first sample only establishes the
    // baseline and publishes no reading.
    pwrNeedRebaseline = true;
    workLoop->addEventSource(timerEvent_main);
    workLoop->addEventSource(timerEvent_tempe);
    timerEvent_main->setTimeoutMS(1);
    timerEvent_tempe->setTimeoutMS(HF_TEMP_SAMPLE_PERIOD);
}

void AMDRyzenCPUPowerManagement::stopWorkLoop() {
//    IOLog("AMDCPUSupport::startWorkLoop stopping timer");

    
    workLoop->disableAllEventSources();
    serviceInitialized = false;
}

void AMDRyzenCPUPowerManagement::resumeWorkLoop() {
    workLoop->enableAllEventSources();
}

bool AMDRyzenCPUPowerManagement::start(IOService *provider){
    
    bool success = IOService::start(provider);
    if(!success){
        IOLog("AMDCPUSupport::start failed to start. :(\n");
        return false;
    }
    
    disablePrivilegeCheck = checkKernelArgument("-amdpnopchk");
    
    uint32_t cpuid_eax = 0;
    uint32_t cpuid_ebx = 0;
    uint32_t cpuid_ecx = 0;
    uint32_t cpuid_edx = 0;
    CPUInfo::getCpuid(0, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start got CPUID: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    if(cpuid_ebx != CPUInfo::signature_AMD_ebx
       || cpuid_ecx != CPUInfo::signature_AMD_ecx
       || cpuid_edx != CPUInfo::signature_AMD_edx){
        IOLog("AMDCPUSupport::start no AMD signature detected, failing..\n");
        
        return false;
    }
    
    CPUInfo::getCpuid(1, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuFamily = ((cpuid_eax >> 20) & 0xff) + ((cpuid_eax >> 8) & 0xf);
    // CPUID Fn0000_0001: model = extended model (EAX[19:16]) << 4 | base model
    // (EAX[7:4]). The old code *added* the two fields, so every model with a
    // non-zero extended nibble (0x50 on Cezanne, for instance) was decoded as a
    // different model and never matched the entries in tctl_offset_table[].
    cpuModel = (((cpuid_eax >> 16) & 0xf) << 4) | ((cpuid_eax >> 4) & 0xf);
    
    //Only 17h Family are supported offically by now.
    // EDIT: add check bypass for 19h Zen 3 processors
    cpuSupportedByCurrentVersion = (cpuFamily == 0x17 || cpuFamily == 0x19)? 1 : 0;
    IOLog("AMDCPUSupport::start Family %02Xh, Model %02Xh\n", cpuFamily, cpuModel);
    
    CPUInfo::getCpuid(0x80000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    // 0.7.6: copy-paste bug fixed. CPUID Fn80000005: ECX[31:24] is the L1 data cache
    // size per core in KB and EDX[31:24] is the L1 instruction cache size per core in
    // KB. The old code added ECX twice (data cache doubled, instruction cache missing),
    // so the L1 size printed in the boot log was wrong.
    cpuCacheL1_perCore = (cpuid_ecx >> 24) + (cpuid_edx >> 24);
    
    
    CPUInfo::getCpuid(0x80000006, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpuCacheL2_perCore = (cpuid_ecx >> 16);
    cpuCacheL3 = (cpuid_edx >> 18) * 512;
    IOLog("AMDCPUSupport::start L1: %u, L2: %u, L3: %u\n",
          cpuCacheL1_perCore, cpuCacheL2_perCore, cpuCacheL3);
    
    
    CPUInfo::getCpuid(0x80000007, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    cpbSupported = (cpuid_edx >> 9) & 0x1;

    
    CPUInfo::getCpuid(0x00000005, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    IOLog("AMDCPUSupport::start CPUID MWait: %X %X %X %X\n", cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx);
    
    uint32_t nameString[12]{};
    CPUInfo::getCpuid(0x80000002, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[0] = cpuid_eax; nameString[1] = cpuid_ebx; nameString[2] = cpuid_ecx; nameString[3] = cpuid_edx;
    CPUInfo::getCpuid(0x80000003, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[4] = cpuid_eax; nameString[5] = cpuid_ebx; nameString[6] = cpuid_ecx; nameString[7] = cpuid_edx;
    CPUInfo::getCpuid(0x80000004, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
    nameString[8] = cpuid_eax; nameString[9] = cpuid_ebx; nameString[10] = cpuid_ecx; nameString[11] = cpuid_edx;
    
    IOLog("AMDCPUSupport::start Processor: %s))\n", (char*)nameString);
    
    //Check tctl temperature offset
    for(int i = 0; i < TCTL_OFFSET_TABLE_LEN; i++){
        const TempOffset *to = tctl_offset_table + i;
        if(cpuFamily == to->model && strstr((char*)nameString, to->id)){
            
            tempOffset = (float)to->offset;
            break;
        }
    }
    
    uint64_t rapl = 0;
    // 0.7.6 (Round 2 P2): this used to be panic(), i.e. a kernel panic on the start()
    // path. That contradicts the "degrade when something is missing" rule: failing to
    // read the RAPL unit only affects package power readings and must not take the
    // machine down. Degrade to "power measurement unavailable" (updatePackageEnergy()
    // skips its work).
    if(!read_msr(kMSR_RAPL_PWR_UNIT, &rapl)){
        IOLog("AMDCPUSupport::start WARN: unable to read power unit, "
              "package power measurement disabled\n");
        raplValid = false;
    } else {
        raplValid = true;
    }
    
    // Bit fields of AMD MSRC001_0299 (RAPL Power Unit):
    //   TU [19:16] / ESU [12:8] / PU [3:0]
    // The masks below match that layout (ESU is 5 bits, TU is 4 bits).
    pwrTimeUnit = pow((double)0.5, (double)((rapl >> 16) & 0xf));
    pwrEnergyUnit = pow((double)0.5, (double)((rapl >> 8) & 0x1f));
//    IOLog("a %lld\n", (long long)(pwrTimeUnit * 10000000000));
//    IOLog("b %lld\n", (long long)(pwrEnergyUnit * 10000000000));
    
    fetchOEMBaseBoardInfo();
    
//    if(!CPUInfo::getCpuTopology(cpuTopology)){
//        IOLog("AMDCPUSupport::start unable to get CPU Topology.\n");
//    }
//    IOLog("AMDCPUSupport::start got %hhu CPU(s): Physical Count: %hhu, Logical Count %hhu.\n",
//          cpuTopology.packageCount, cpuTopology.totalPhysical(), cpuTopology.totalLogical());
//
//    totalNumberOfPhysicalCores = cpuTopology.totalPhysical();
//    totalNumberOfLogicalCores = cpuTopology.totalLogical();
    
    
    IOLog("AMDCPUSupport::start trying to init PCI service...\n");
    if(!getPCIService()){
        IOLog("AMDCPUSupport::start no PCI support found, failing...\n");
        return false;
    }
    
// 0.7.6: there used to be a spin here waiting for pmRyzen_symtable_ready. The
// variable was only ever written (the code waiting on it had long been commented
// out), so it was removed together with the cleanup.
    
    void *safe_wrmsr = pmRyzen_symtable._wrmsr_carefully;
    if(!safe_wrmsr){
        IOLog("AMDCPUSupport::start WARN: Can't find _wrmsr_carefully, proceeding with unsafe wrmsr\n");
    } else {
        wrmsr_carefully = (int(*)(uint32_t,uint32_t,uint32_t)) safe_wrmsr;
    }

    void *_kunc_alert = pmRyzen_symtable._KUNCUserNotificationDisplayAlert;
    IOLog("kunc_alert %p\n", kunc_alert);
    if(!_kunc_alert){
        IOLog("AMDCPUSupport::start WARN: Can't find _KUNCUserNotificationDisplayAlert.\n");
    } else {
        kunc_alert =
        (kern_return_t(*)(int,unsigned,const char*,const char*,const char*,
        const char*,const char*,const char*,const char*,const char*,unsigned*))_kunc_alert;
    }

    cpu_to_processor = (processor_t(*)(int))pmRyzen_symtable._cpu_to_processor;
    if(!pmRyzen_symtable._cpu_to_processor){
        IOLog("AMDCPUSupport::start WARN: Can't find _cpu_to_processor\n");
    }

    // 0.7.6: _tscFreq is a pointer to a kernel variable. Dereferencing it when the
    // symbol is missing crashed the kernel during start() (null pointer dereference).
    // It now keeps the default value and disables the package power measurement that
    // depends on it.
    // 0.7.6 (Round 4 M5): the *symbol* existing does not mean its *value* is valid -
    // when xnuTSCFreq == 0 the "seconds" value in updatePackageEnergy() becomes +inf
    // and the result is a silently wrong 0 W. An invalid value is treated as
    // "frequency unavailable" too.
    if(pmRyzen_symtable._tscFreq){
        xnuTSCFreq = *((uint64_t*)pmRyzen_symtable._tscFreq);
        tscFreqValid = (xnuTSCFreq > 0);
        if(!tscFreqValid)
            IOLog("AMDCPUSupport::start WARN: _tscFreq is 0, package power measurement disabled\n");
    } else {
        IOLog("AMDCPUSupport::start WARN: Can't find _tscFreq, package power measurement disabled\n");
    }

    // 0.7.6 (T8 / O26): mirror the -amdpdbg switch into the PM side (the C file cannot
    // see the provider's ADDPR(debugEnabled)). This must happen before
    // pmRyzen_init(), because the boot-time P1 rewrite is logged based on it.
    pmRyzen_debug_enabled = ADDPR(debugEnabled) ? 1 : 0;

    // 0.7.6: when pmRyzen_init() fails (PM symbols missing) the service must not be
    // published with 0 physical cores: user space divides logical by physical and
    // would crash the kernel (integer divide by zero). Fail start() instead, so the
    // app reports that the kext is not loaded.
    if(!pmRyzen_init(this)){
        IOLog("AMDCPUSupport::start failed: PM registration unavailable\n");
        return false;
    }
    
    totalNumberOfLogicalCores = pmRyzen_num_logi;
    totalNumberOfPhysicalCores = pmRyzen_num_phys;
    
    if(totalNumberOfPhysicalCores == 0 || totalNumberOfLogicalCores == 0
       || totalNumberOfPhysicalCores > CPUInfo::MaxCpus){
        IOLog("AMDCPUSupport::start failed: bad CPU topology (%u physical / %u logical)\n",
              totalNumberOfPhysicalCores, totalNumberOfLogicalCores);
        // 0.7.6 (Round 2 F7): pmRyzen_init() has already registered pmDispatch at this
        // point. Returning false directly would leave a pmRyzen_machine_idle callback on
        // the XNU side pointing at a half-initialized, all-zero pmRyzen_cpus[] object
        // => unregister explicitly.
        pmRyzen_stop();
        return false;
    }

    IOLog("AMDCPUSupport::start, Physical Count: %u, Logical Count %u.\n",
              totalNumberOfPhysicalCores, totalNumberOfLogicalCores);

    workLoop = IOWorkLoop::workLoop();
    initWorkLoop();

	// Whether CPB should be disabled at boot (Info.plist property CPBStatus)
    uint8_t customValue = 0;
    OSNumber *arg;
    arg = OSDynamicCast(OSNumber, getProperty("CPBStatus"));
    if(arg==NULL){
        customValue = 0;
    } else {
        customValue = arg->unsigned8BitValue();
    }
    // sleepState.cpb caches the DESIRED CPB state (what the wake path should
    // (re)apply), not necessarily the current hardware state: if the
    // setCPBState() write below fails, restoreWakeState() retries applying
    // this value on the next wake.
    sleepState.cpb = customValue>=1;
    // The value used here is not a frequency but an index into the P-state table
    arg = OSDynamicCast(OSNumber, getProperty("SpeedID"));
    if(arg==NULL){
        customValue = 0;
    } else {
        customValue = arg->unsigned8BitValue();
    }

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, powerStates, kNrOfPowerStates);
  
    // A custom frequency is configured and CPB is disabled
    if(!sleepState.cpb) {
        if(!setCPBState(sleepState.cpb) && ADDPR(debugEnabled))
            IOLog("AMDCPUSupport::start: failed to apply CPBStatus property\n");
        if(customValue>0) {
            setPMPStateLimit(0);
            sleepState.PStateCtl = customValue;
            applyPowerControl(sleepState.PStateCtl);
        } else {
            setPMPStateLimit(1);
        }
    }
    
    return success;
}

void AMDRyzenCPUPowerManagement::stop(IOService *provider){
    pmRyzen_stop();
    
    IOLog("AMDCPUSupport stopped\n");
    
    stopWorkLoop();
    timerEvent_main->cancelTimeout();
    workLoop->removeEventSource(timerEvent_main);
    timerEvent_main->release();
    
    if(superIO){
        for (int i = 0; i < superIO->getNumberOfFans(); i++) {
            superIO->setDefaultFanControl(i);
        }

        delete superIO;
    }

    PMstop();

    IOService::stop(provider);

}

IOReturn AMDRyzenCPUPowerManagement::setPowerState(unsigned long powerStateOrdinal, IOService* provider) {
    if (0 == powerStateOrdinal) {
        // Going to sleep.
        // 0.7.5: do NOT call IOLog here at all — even guarded by
        // ADDPR(debugEnabled), IOLog takes the console output lock,
        // which is a different ticket spinlock from the one that
        // panicked on 2026-08-26 (cpu_lock) but is still a hot global
        // lock while PM is driving 16 CPUs into sleep. Keep the path
        // fully lock-free; rely on `-amdpdbg` logs emitted from the
        // WorkLoop for any forensic trace of sleep/wake.
        sleepState.sleep = true;
        // 0.7.5: removed `getCPBState()` and `dumpPstate()` from the sleep
        // path. Both called `rdmsr_carefully` -> `cpu_lock` while PM was
        // concurrently taking it on other CPUs to drive sleep, causing the
        // ticket spinlock panic on 2026-08-26.
        //
        // sleepState.cpb is kept in sync by setCPBState() on every explicit
        // change, so it always holds the last requested CPB state.
        //
        // The P-state definitions are snapshotted without touching any MSR:
        // PStateDef_perCore is maintained by dumpPstate() (work loop init and
        // the user client writePstate() path), so it already holds the current
        // table — including the user's custom frequency settings. A plain
        // memory copy keeps this path completely lock free.
        stopWorkLoop();
        memcpy(sleepState.pstate, PStateDef_perCore, sizeof(sleepState.pstate));
    } else if (1 == powerStateOrdinal && sleepState.sleep) {
        // Waking up — see sleep branch above for the no-IOLog rationale.
        sleepState.sleep = false;
        restoreWakeState();
        // 0.7.6 (T8 / O24): raise the "energy baseline must be re-established" flag.
        // After S3 the RAPL counter may restart from 0, so subtracting the pre-sleep
        // baseline would give a huge delta => a huge spike in the first power reading
        // after wake.
        //
        // 0.7.6 (Round 4 M3): the flag is raised in the *wake* branch rather than the
        // sleep branch - the work loop is still disabled at this point, so no tick can
        // consume the flag first. When it was raised in the sleep branch, a work-loop
        // thread already inside updatePackageEnergy() could overwrite the true written by
        // the PM thread with false => the flag was lost => the spike still appeared (a
        // window of a few microseconds against a 50-1200 ms tick, so roughly 1e-6 per
        // sleep, but the consequence is exactly the symptom O24 exists to remove).
        pwrNeedRebaseline = true;
        resumeWorkLoop();
    }

    return kIOPMAckImplied;
}

void AMDRyzenCPUPowerManagement::fetchOEMBaseBoardInfo(){
    auto efiRT = EfiRuntimeServices::get();
    uint32_t att = 0;
    uint64_t sizee = BASEBOARD_STRING_MAX;
    uint64_t efistat;
    efistat = efiRT->getVariable(OC_OEM_VENDOR_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardVender);
    
    sizee = BASEBOARD_STRING_MAX;
    efistat = efiRT->getVariable(OC_OEM_BOARD_VARIABLE_NAME, &EfiRuntimeServices::LiluVendorGuid,
                                 &att, &sizee, boardName);
    boardInfoValid = efistat == EFI_SUCCESS;
    IOLog("MB: %s %s\n", boardName, boardVender);
}

bool AMDRyzenCPUPowerManagement::read_msr(uint32_t addr, uint64_t *value){
    
    uint32_t lo, hi;
    //    IOLog("AMDCPUSupport lalala \n");
    int err = rdmsr_carefully(addr, &lo, &hi);
    //    IOLog("AMDCPUSupport rdmsr_carefully %d\n", err);
    
    if(!err) *value = lo | ((uint64_t)hi << 32);
    
    return err == 0;
}

bool AMDRyzenCPUPowerManagement::write_msr(uint32_t addr, uint64_t value){
    if(wrmsr_carefully){
        uint32_t lo = value & 0xffffffff;
        uint32_t hi = value >> 32;
        return (*wrmsr_carefully)(addr, lo, hi) == 0;
    }
    
    //Fall back with unsafe method
    wrmsr64(addr, value);
    
    //If failed, we've already panic and starting reboot. So just return true.
    return true;
}

// 0.7.6: single entry point for user alerts.
//
// kunc_alert stays nullptr when _KUNCUserNotificationDisplayAlert cannot be
// resolved. The user client used to dereference it unconditionally in two places
// (hasPrivilege() and externalMethod()) - combined with the kextloadAlerts++ in
// init() (incremented on every retry while the symbol is missing), the very first
// user-space call could crash the kernel.
//
// The null check now happens in one place:
//   - alert available: show it, the response is valid, return true;
//   - alert unavailable: degrade to IOLog, set the response to UINT_MAX and
//     return false. "The user was not asked" must never be read as "the user
//     agreed", so the caller has to degrade on its own.
bool AMDRyzenCPUPowerManagement::presentAlert(const char *title, const char *body,
                                               const char *btn1, const char *btn2, const char *btn3,
                                               unsigned int *response){
    // 0.7.6 (Round 2 F4): the sentinel is UINT_MAX rather than 0 - 0 is a *valid
    // button index* (the CFUserNotification default button). Using 0 as "the user
    // was not asked" would let any caller that ignores the bool return value read
    // "not asked" as "the user pressed the default button".
    if(response) *response = UINT_MAX;
    
    if(!kunc_alert){
        IOLog("AMDCPUSupport: user alert unavailable (%s): %s\n",
              title ? title : "", body ? body : "");
        return false;
    }
    
    (*kunc_alert)(0, 0, NULL, NULL, NULL, title, body, btn1, btn2, btn3, response);
    return true;
}

void AMDRyzenCPUPowerManagement::registerRequest(){
    uint32_t now = (uint32_t)(getCurrentTimeNs() / 1000000);
    
    estimatedRequestTimeInterval = now - timeOfLastMissedRequest;
    timeOfLastMissedRequest = now;
}

// 0.7.6: updateClockSpeed() was removed - it had no call sites (dead code).
// effFreq_perCore is maintained by calculateEffectiveFrequency() (the APERF/MPERF
// difference) and is no longer derived from the P-state status register.

void AMDRyzenCPUPowerManagement::calculateEffectiveFrequency(uint8_t physical){
    
    uint32_t APERF_lo, APERF_hi;
    uint32_t MPERF_lo, MPERF_hi;
    
    /**
     * The effective frequency interface provides +/- 50MHz accuracy if the following constraints are met:
     * • Effective frequency is read at most one time per millisecond.
     * • When reading or writing Core::X86::Msr::MPERF and Core::X86::Msr::APERF software executes only
     *  MOV instructions, and no more than 3 MOV instructions, between the two RDMSR or WRMSR
     *  instructions.
     * • Core::X86::Msr::MPERF and Core::X86::Msr::APERF are invalid if an overflow occurs.
    */
    __asm__ volatile("movl $0xe8, %%ecx;"
                     "rdmsr;"
                     "movl %%eax, %0;"
                     "movl %%edx, %1;"
                     "movl $0xe7, %%ecx;"
                     "rdmsr;"
                     : "=r"(APERF_lo), "=r"(APERF_hi), "=a"(MPERF_lo), "=d"(MPERF_hi)
                     :
                     : "%ecx"
                    );
    
    uint64_t APERF = APERF_lo | ((uint64_t)APERF_hi << 32);
    uint64_t MPERF = MPERF_lo | ((uint64_t)MPERF_hi << 32);;

    if(!freqCountersSeeded[physical]){
        // A failed boot-time seed (initWorkLoop) left the baselines at 0;
        // seed from this first successful read instead of reporting one
        // sample scaled from cumulative-since-reset counters.
        lastAPERF_perCore[physical] = APERF;
        lastMPERF_perCore[physical] = MPERF;
        freqCountersSeeded[physical] = true;
        return;
    }
        
    uint64_t lastAPERF = lastAPERF_perCore[physical];
    uint64_t lastMPERF = lastMPERF_perCore[physical];
    
    lastAPERF_perCore[physical] = APERF;
    lastMPERF_perCore[physical] = MPERF;
    //If an overflow of either the MPERF or APERF register occurs between read of last MPERF and
    //read of last APERF, the effective frequency calculated in is invalid.
    if(APERF <= lastAPERF || MPERF <= lastMPERF) {
//        IOLog("AMDCPUSupport::calculateEffectiveFrequency: frequency is invalid!!!");
        lastAPERF_perCore[physical] = APERF;
        lastMPERF_perCore[physical] = MPERF;
        return;
    }
    
    float freqP0 = PStateDefClock_perCore[0];
    
    // A P0 definition we could not read leaves the cached clock at 0. Scaling
    // by it would report a bogus frequency, so keep the last good value.
    if(freqP0 <= 0) return;
    
    uint64_t deltaAPERF = APERF - lastAPERF;
//    float effFreq = ((float)deltaAPERF / (float)(MPERF - lastMPERF)) * freqP0;
    
//    effFreq_perCore[physical] = effFreq;
    uint64_t deltaMPERF = MPERF - lastMPERF;
    if (deltaMPERF != 0) {
        float effFreq = ((float)deltaAPERF / (float)(deltaMPERF)) * freqP0;
        effFreq_perCore[physical] = effFreq;
    }
}

void AMDRyzenCPUPowerManagement::updateInstructionDelta(uint8_t cpu_num){
    uint64_t insCount;
    
    if(!read_msr(kMSR_PERF_IRPC, &insCount))
        return;
    
    
    //Skip if overflowed
    if(lastInstructionDelta_perCore[cpu_num] > insCount) return;
    
//    uint64_t delta = insCount - lastInstructionDelta_perCore[cpu_num];
    instructionDelta_perCore[cpu_num] = insCount - lastInstructionDelta_perCore[cpu_num];
    
    lastInstructionDelta_perCore[cpu_num] = insCount;
    
    //write_msr(kMSR_PERF_IRPC, 0);
    
    
    //Calculate load index
//    float estimatedInstRet = (effFreq_perCore[cpu_num] * 1000000);
//    estimatedInstRet = estimatedInstRet * (actualUpdateTimeInterval * 0.001);
//    float index = (float)delta / estimatedInstRet;
//
//    float growth = 3200;
//    loadIndex_PerCore[cpu_num] = log10f(min(index,1) * growth) / log10f(growth);
}

void AMDRyzenCPUPowerManagement::applyPowerControl(uint8_t pstate){
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        uint8_t pstate;
    };

    Args args = { this, pstate };

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        auto provider = args->provider;
        auto pstate = args->pstate;

        if(pstate != 255) provider->PStateCtl = pstate;
        provider->sleepState.PStateCtl = provider->PStateCtl;
        provider->write_msr(kMSR_PSTATE_CTL, (uint64_t)(provider->PStateCtl & 0x7));
    }, nullptr, &args);
}
uint8_t AMDRyzenCPUPowerManagement::getPowerControl(){
    uint64_t val;
    read_msr(kMSR_PSTATE_CTL, &val);
    return val & 0x7;
}

bool AMDRyzenCPUPowerManagement::setCPBState(bool enabled){
    if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState enabled is %s\n", enabled?"true":"false");

    if(!cpbSupported) return false;

    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig)){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState: failed to read HWCR\n");
        return false;
    }
    
    if(enabled){
        hwConfig &= ~(1 << 25);
    } else {
        hwConfig |= (1 << 25);
    }
    
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        const uint64_t* hwConfig;
        bool writeOk;
    };

    // Report back whether every CPU accepted the write: only a fully applied
    // change may update the cached state the wake path restores from.
    // (Concurrent false stores to writeOk are identical bytes, so the shared
    // flag cannot tear on x86.)
    Args args = { this, &hwConfig, true };

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        if(!args->provider->write_msr(kMSR_HWCR, *args->hwConfig))
            args->writeOk = false;
    }, nullptr, &args);

    if(!args.writeOk){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::setCPBState: failed to write HWCR\n");
        return false;
    }

    // Keep the cached CPB state in sync with what was actually requested.
    // restoreWakeState() decides the CpbDis bit from this value, so without
    // this update the wake path would restore the boot-time default instead
    // of the last explicitly chosen state.
    sleepState.cpb = enabled;

    return true;
}

bool AMDRyzenCPUPowerManagement::getCPBState(){
    uint64_t hwConfig;
    if(!read_msr(kMSR_HWCR, &hwConfig)){
        if (ADDPR(debugEnabled)) IOLog("AMDCPUSupport::getCPBState: failed to read HWCR\n");
        // Fall back to the last state we know about. setCPBState() keeps this
        // in sync with every explicit change, so it reflects the last requested
        // state even if the hardware read fails.
        return sleepState.cpb;
    }

    return !((hwConfig >> 25) & 0x1);
}

inline float AMDRyzenCPUPowerManagement::getPackageTemp() {
    // Fallback used whenever the register read yields nothing usable: keep the
    // last published package temperature, or a neutral 45 C before the first
    // sample. Publishing 0 C (or a value derived from 0xFFFFFFFF) makes sensor
    // apps show a bogus reading, and some of them treat it as a critical value.
    const float fallback = (PACKAGE_TEMPERATURE_perPackage[0] > 0)
        ? PACKAGE_TEMPERATURE_perPackage[0] : 45.0f;

    // getPCIService() can fail (see start()), and this runs on the work loop.
    if(!fIOPCIDevice) return fallback;

    IOPCIAddressSpace space;
    space.bits = 0x00;

    fIOPCIDevice->configWrite32(space, (UInt8)kFAMILY_17H_PCI_CONTROL_REGISTER, (UInt32)kF17H_M01H_THM_TCON_CUR_TMP);
    uint32_t temperature = fIOPCIDevice->configRead32(space, kFAMILY_17H_PCI_CONTROL_REGISTER + 4);

    // A failed config read returns all ones, and 0 is not a plausible Tctl value.
    if(temperature == 0xFFFFFFFF || temperature == 0) return fallback;

    bool tempOffsetFlag = (temperature & kF17H_TEMP_OFFSET_FLAG) != 0;
    temperature = (temperature >> 21) * 125;

    float t = temperature * 0.001f;

    t -= tempOffset;

    if (tempOffsetFlag)
        t -= 49.0f;

    // Anything outside this range is a decode error, not a real temperature.
    if(t <= 0.0f || t > 120.0f) return fallback;

    return t;
}

void AMDRyzenCPUPowerManagement::updatePackageTemp(){
    float sum = 0;
    for (int i = 0; i < HF_TEMP_SAMPLE_LEN; i++) sum += tempSamples[i];
    PACKAGE_TEMPERATURE_perPackage[0] = sum * HF_TEMP_SAMPLE_LENREP;
}

void AMDRyzenCPUPowerManagement::updatePackageEnergy(){
    
    // 0.7.6: when _tscFreq is missing xnuTSCFreq keeps its default value (1) and the
    // computed "seconds" is garbage, so the power reading is meaningless. Do not
    // update at all in that case and keep the previous reading. The same applies when
    // the RAPL unit register could not be read (raplValid == false).
    if(!tscFreqValid || !raplValid) return;
    
    uint64_t ctsc = rdtsc64();

    uint64_t msr_value_buf = 0;
    // 0.7.6 (T8 / O12): check the read_msr() return value. It used to leave
    // msr_value_buf at 0 on failure => energyValue = 0 => the wrap-around branch was
    // taken => delta became astronomically large => a huge phantom spike in the power
    // reading (and the baseline was written as 0, so the next read spiked again).
    // A failed read now skips this sample: the previous uniPackageEnergy is kept and
    // the baseline is left untouched.
    if(!read_msr(kMSR_PKG_ENERGY_STAT, &msr_value_buf)){
        if(ADDPR(debugEnabled))
            IOLog("AMDCPUSupport::updatePackageEnergy: failed to read RAPL energy, sample skipped\n");
        return;
    }

    uint32_t energyValue = (uint32_t)(msr_value_buf & 0xffffffff);

    // 0.7.6 (T8 / O24): re-establish the baseline after sleep/wake. After S3 the RAPL
    // counter may restart from 0, so subtracting the pre-sleep baseline would give a
    // huge delta. The sample that re-establishes the baseline publishes no power
    // value (the previous reading is kept); it only aligns the baseline with the
    // current counter value.
    if(pwrNeedRebaseline){
        pwrNeedRebaseline = false;
        lastUpdateEnergyValue = energyValue;
        pwrLastTSC = ctsc;
        if(ADDPR(debugEnabled))
            IOLog("AMDCPUSupport::updatePackageEnergy: energy baseline re-established after sleep\n");
        return;
    }

    // 0.7.6 (T8 / O12): let the 32-bit counter wrap naturally as unsigned. The old
    // expression `UINT32_MAX - lastUpdateEnergyValue` dropped the `1 + energyValue`
    // segment on a real wrap-around.
    uint32_t energyDelta = (uint32_t)(energyValue - lastUpdateEnergyValue);

    double seconds = (ctsc - pwrLastTSC) / (double)(xnuTSCFreq);
    // 0.7.6 (T8 / O24): divide-by-zero guard - when the TSC has not advanced (two
    // samples taken back to back) seconds is 0 and the result is +inf, a meaningless
    // value for callers. xnuTSCFreq == 0 is already excluded by tscFreqValid in
    // start() (a present symbol does not mean a valid value, see Round 4 M5).
    if(seconds <= 0.0) return;

    // 0.7.6 (T8 / O12): corrected conversion, aligned with LHM's Amd17Cpu.cs:
    //     power(W) = dESU * 2^-ESU(J) / dseconds
    // It does not include pwrTimeUnit - TU (time granularity 2^-TU seconds) only
    // matters when the timestamp comes from RAPL itself, while the time used here
    // comes from the TSC and is unrelated to TU.
    double eCorrected = (pwrEnergyUnit * (double)energyDelta) / seconds;

    // 0.7.6 (T8 / O12): the old formula multiplied by `pwrTimeUnit * 1000`, a factor
    // with no dimensional justification (it is not in the LHM reference) that makes
    // the current power readings systematically low. Per decision Q1 (b) the *outward
    // value stays the old one*: the corrected value is only computed alongside it and
    // logged as a dual-value comparison under -amdpdbg for a week, then switched over
    // in a separate commit once confirmed.
    //
    // The parenthesisation matches the old code exactly
    // (`e *= pwrTimeUnit * 1000`): floating point multiplication does not associate,
    // so (e * pwrTimeUnit) * 1000 and e * (pwrTimeUnit * 1000) can differ in the last
    // bit - the outward value must be bit-identical to 0.7.5 for the comparison to
    // mean anything.
    double eLegacy = eCorrected * (pwrTimeUnit * 1000.0);

    uniPackageEnergy = eLegacy;

    // 0.7.6 (T8 / O12, Q1=b): dual-value comparison log (-amdpdbg, once per 30 s)
    if(ADDPR(debugEnabled)){
        uint64_t nowNs = getCurrentTimeNs();
        if(nowNs - lastPowerDiagNs > 30ULL * 1000000000ULL){
            lastPowerDiagNs = nowNs;
            IOLog("AMDCPUSupport::updatePackageEnergy: legacy %.3f W | corrected %.3f W "
                  "(delta %+.3f W, ratio %.4f)\n",
                  eLegacy, eCorrected, eCorrected - eLegacy,
                  eLegacy != 0.0 ? eCorrected / eLegacy : 0.0);
        }
    }

    lastUpdateEnergyValue = energyValue;
    pwrLastTSC = rdtsc64();
}


bool AMDRyzenCPUPowerManagement::dumpPstate(uint64_t buf[8], bool allowLog){

    bool allOk = true;
    for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
        uint64_t msr_value_buf = 0;
        if (!read_msr(kMSR_PSTATE_0 + i, &msr_value_buf)) {
            // Never panic on the PM callback path: a failed rdmsr during a
            // sleep/wake transition must not take the whole machine down.
            // Store 0 so writePstate() skips this entry on restore.
            //
            // The cached PStateDef_* entries are deliberately left untouched
            // here: they hold the last known good definition, which is still
            // what the sleep snapshot and the App's P-state table should show.
            // Zeroing them would drop a perfectly valid definition on a
            // transient read failure.
            if (allowLog && ADDPR(debugEnabled)) IOLog("AMDCPUSupport::dumpPstate: failed to read P-state %u\n", i);
            if(buf != nullptr) buf[i] = 0;
            allOk = false;
            continue;
        }
        
        uint32_t eax = (uint32_t)(msr_value_buf & 0xffffffff);
        
        // CpuVid [21:14]
        // CpuDfsId [13:8]
        // CpuFid [7:0]
        int curCpuDfsId = (int)((eax >> 8) & 0x3f);
        int curCpuFid = (int)(eax & 0xff);
        
        float clock = (float)((float)curCpuFid / (float)curCpuDfsId * 200.0);
        
        PStateDef_perCore[i] = msr_value_buf;
        PStateDefClock_perCore[i] = clock;
        if (allowLog && ADDPR(debugEnabled)) IOLog("AMDCPUSupport::dumpPstate: Index: %u, value: %llu\n", i, msr_value_buf);
        if(buf != nullptr) buf[i] = msr_value_buf;
    };

    return allOk;
}

void AMDRyzenCPUPowerManagement::writePstate(const uint64_t *buf){
    
    //A bit hacky but at least works for now.
    void* args[] = {this, (void*)buf};
    
    
    mp_rendezvous(nullptr, [](void *obj) {
        auto v = static_cast<uint64_t*>(((uint64_t**)obj)[1]);
        auto provider = static_cast<AMDRyzenCPUPowerManagement*>(*((AMDRyzenCPUPowerManagement**)obj));

        for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
            uint64_t def = v[i];
            
            uint64_t curCpuDfsId = ((def >> 8) & 0x3f);
            uint64_t curCpuFid = (def & 0xff);
            if(!def || !curCpuDfsId || !curCpuFid)
                continue;
            
            provider->write_msr(provider->kMSR_PSTATE_0 + i, def);
            
        }
    
    }, nullptr, args);

    // Refresh the cached P-state table on the calling thread, OUTSIDE the
    // rendezvous barrier: dumpPstate -> read_msr takes cpu_lock (and may
    // IOLog under -amdpdbg), which is the exact deadlock/assert hazard the
    // wake path was hardened against. P-state definition MSRs are uniform
    // across the package, so reading them on the calling CPU is equivalent
    // to reading them on the master inside the barrier.
    pstateDumpRetryLeft = 20;
    // allowLog=true is deliberate here: this runs on the user-client thread
    // with interrupts enabled, unlike the rendezvous-internal dumps.
    pstateDumpPending = !dumpPstate(nullptr, true);
        
}

void AMDRyzenCPUPowerManagement::restoreWakeState(){
    struct Args {
        AMDRyzenCPUPowerManagement* provider;
        const uint64_t* pstateBuf;
        uint64_t hwcr;
        uint8_t pstateCtl;
        bool updateHWCR;
        bool anyPstate;
    };

    // Merge what used to be three separate all-core synchronizations per wake
    // (setCPBState / writePstate / applyPowerControl) into a single
    // mp_rendezvous. Each rendezvous requires every CPU to respond, which is a
    // deadlock hazard while cores are still coming back online.

    // Evaluate the snapshot on this thread, before the barrier: if no P-state
    // definition is valid there is nothing to restore, and selecting a
    // P-state index (e.g. P0) without restoring the register file would be
    // worse than leaving the current selection alone.
    bool anyPstate = false;
    for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
        uint64_t def = sleepState.pstate[i];
        if(def && ((def >> 8) & 0x3f) && (def & 0xff)) anyPstate = true;
    }

    Args args = { this, sleepState.pstate, 0, sleepState.PStateCtl, false, anyPstate };

    if (cpbSupported) {
        // TODO(0.7.6): this read_msr still takes cpu_lock on the master
        // thread while 15 other CPUs are spinning in the rendezvous
        // barrier below. Same shape as the 2026-08-26 panic but in the
        // wake path. Fix by caching HWCR at start() and refreshing only
        // when a deliberate CPB change is requested.
        //
        // 0.7.6: rewriting the firmware P1 definition from the wake path is the same
        // shape as the 2026-08-26 panic (rdmsr_carefully -> cpu_lock preemption), just on
        // the wake path instead of the sleep path. The remaining open questions about this
        // window are tracked together with the planned fix: cache HWCR in start() and
        // refresh it only when a deliberate CPB change is requested.
        uint64_t hwcr;
        if (read_msr(kMSR_HWCR, &hwcr)) {
            if (sleepState.cpb) {
                hwcr &= ~((uint64_t)1 << 25);
            } else {
                hwcr |= ((uint64_t)1 << 25);
            }
            args.hwcr = hwcr;
            args.updateHWCR = true;
        } else if (ADDPR(debugEnabled)) {
            IOLog("AMDCPUSupport::restoreWakeState: failed to read HWCR, skipping CPB restore\n");
        }
    }

    mp_rendezvous(nullptr, [](void *arg) {
        auto args = static_cast<Args*>(arg);
        auto provider = args->provider;

        if (args->updateHWCR)
            provider->write_msr(kMSR_HWCR, args->hwcr);

        for (uint32_t i = 0; i < kMSR_PSTATE_LEN; i++) {
            uint64_t def = args->pstateBuf[i];

            uint64_t curCpuDfsId = ((def >> 8) & 0x3f);
            uint64_t curCpuFid = (def & 0xff);
            if(!def || !curCpuDfsId || !curCpuFid)
                continue;

            provider->write_msr(provider->kMSR_PSTATE_0 + i, def);
        }

        if(args->anyPstate)
            provider->write_msr(kMSR_PSTATE_CTL, (uint64_t)(args->pstateCtl & 0x7));

        // 0.7.5: removed the master-side `dumpPstate(nullptr)` call here.
        // dumpPstate -> read_msr takes cpu_lock on the calling thread, which
        // deadlocked under the same conditions as the sleep-path panic.
        // PStateDef_* refresh now happens in initWorkLoop()'s first
        // mp_rendezvous_no_intrs (next timer fire, < 200 ms window).
    }, nullptr, &args);
}

bool AMDRyzenCPUPowerManagement::initSuperIO(uint16_t *chipIntel){
    
    superIO = nullptr;
    if(!superIO) superIO = ISSuperIONCT668X::getDevice(&savedSMCChipIntel);
    if(!superIO) superIO = ISSuperIONCT67XXFamily::getDevice(&savedSMCChipIntel);
    if(!superIO) superIO = ISSuperIOIT86XXEFamily::getDevice(&savedSMCChipIntel);
    
    *chipIntel = savedSMCChipIntel;
    
    return superIO != nullptr;
}

uint32_t AMDRyzenCPUPowerManagement::getPMPStateLimit(){
    return pmRyzen_pstatelimit;
}

void AMDRyzenCPUPowerManagement::setPMPStateLimit(uint32_t state){
    pmRyzen_pstatelimit = min(2, state);
    if(state > 0){
        pmRyzen_PState_reset();
    }
}

uint32_t AMDRyzenCPUPowerManagement::getHPcpus(){
    return pmRyzen_hpcpus;
}

EXPORT extern "C" kern_return_t amdryzencpupm_kern_start(kmod_info_t *, void *) {
    // Report success but actually do not start and let I/O Kit unload us.
    // This works better and increases boot speed in some cases.
    PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
    ADDPR(debugEnabled) = checkKernelArgument("-amdpdbg");
    
//    IOLog("AMDCPUSupport::enter dlinking..\n");
//
//
//    pmRyzen_symtable._wrmsr_carefully = lookup_symbol("_wrmsr_carefully");
//    pmRyzen_symtable._KUNCUserNotificationDisplayAlert = lookup_symbol("_KUNCUserNotificationDisplayAlert");
//    pmRyzen_symtable._cpu_to_processor = lookup_symbol("_cpu_to_processor");
//    pmRyzen_symtable._tscFreq = lookup_symbol("_tscFreq");
//    pmRyzen_symtable._pmDispatch = lookup_symbol("_pmDispatch");
//    pmRyzen_symtable._pmUnRegister = lookup_symbol("_pmUnRegister");
//    pmRyzen_symtable._cpu_NMI_interrupt = lookup_symbol("_cpu_NMI_interrupt");
//    pmRyzen_symtable._NMIPI_enable = lookup_symbol("_NMIPI_enable");
//    pmRyzen_symtable._i386_cpu_IPI = lookup_symbol("_i386_cpu_IPI");
//    IOLog("AMDCPUSupport::enter link finished.\n");
    
    return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t amdryzencpupm_kern_stop(kmod_info_t *, void *) {
    // It is not safe to unload VirtualSMC plugins!
    return KERN_FAILURE;
}



