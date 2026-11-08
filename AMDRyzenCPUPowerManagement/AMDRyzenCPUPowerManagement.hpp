#ifndef AMDRyzenCPUPowerManagement_h
#define AMDRyzenCPUPowerManagement_h

//Support for macOS 10.13
#include "Headers/LegacyHeaders/LegacyIOService.h"

#include <math.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOTimerEventSource.h>


#include <i386/proc_reg.h>
#include <libkern/libkern.h>


#include <Headers/kern_efi.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_time.hpp>


//#include <Headers/kern_api.hpp>
#define LILU_CUSTOM_KMOD_INIT
#define LILU_CUSTOM_IOKIT_INIT
#include <Headers/plugin_start.hpp>
#include "symresolver/kernel_resolver.h"

#include "SuperIO/ISSuperIONCT668X.hpp"
#include "SuperIO/ISSuperIONCT67XXFamily.hpp"
#include "SuperIO/ISSuperIOIT86XXEFamily.hpp"

#include "Headers/pmRyzenSymbolTable.h"

#include <i386/cpuid.h>

#define OC_OEM_VENDOR_VARIABLE_NAME        u"oem-vendor"
#define OC_OEM_BOARD_VARIABLE_NAME         u"oem-board"

#define BASEBOARD_STRING_MAX 64

#define kNrOfPowerStates 2
#define kIOPMPowerOff 0

extern "C" {
#include "pmAMDRyzen.h"

#include "Headers/osfmk/i386/pmCPU.h"
#include "Headers/osfmk/i386/cpu_topology.h"
    

int cpu_number(void);
void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

void mp_rendezvous(void (*setup_func)(void *),
                  void (*action_func)(void *),
                  void (*teardown_func)(void *),
                  void *arg);

void i386_deactivate_cpu(void);
//    int wrmsr_carefully(uint32_t msr, uint64_t val);


int pmRyzen_wrmsr_safe(void *, uint32_t, uint64_t);
uint64_t pmRyzen_rdmsr_safe(void *, uint32_t);



extern pmRyzen_symtable_t pmRyzen_symtable;
};


/**
 * Offset table: https://github.com/torvalds/linux/blob/master/drivers/hwmon/k10temp.c#L78
 */
typedef struct tctl_offset {
    uint8_t model;
    char const *id;
    int offset;
} TempOffset;

typedef struct sleep_state {
    bool sleep;
    bool cpb;
    uint64_t pstate[8];
    uint8_t PStateCtl;
} SleepState;


static IOPMPowerState powerStates[kNrOfPowerStates] = {
   {1, kIOPMPowerOff, kIOPMPowerOff, kIOPMPowerOff, 0, 0, 0, 0, 0, 0, 0, 0},
   {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};


class AMDRyzenCPUPowerManagement : public IOService {
    OSDeclareDefaultStructors(AMDRyzenCPUPowerManagement)
    
public:
    
    char kMODULE_VERSION[12]{};
    
    /**
     *  MSRs supported by AMD 17h CPU from:
     *  https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs
     * and
     * Processor Programming Reference for AMD 17h CPU,
     *
     */
    
    static constexpr uint32_t kCOFVID_STATUS = 0xC0010071;
    static constexpr uint32_t k17H_M01H_SVI = 0x0005A000;
    static constexpr uint32_t kF17H_M01H_THM_TCON_CUR_TMP = 0x00059800;
    static constexpr uint32_t kF17H_M70H_CCD1_TEMP = 0x00059954;
    static constexpr uint32_t kF17H_TEMP_OFFSET_FLAG = 0x80000;
    static constexpr uint32_t kF18H_TEMP_OFFSET_FLAG = 0x60000;
    static constexpr uint8_t kFAMILY_17H_PCI_CONTROL_REGISTER = 0x60;
    static constexpr uint32_t kMSR_HWCR = 0xC0010015;
    static constexpr uint32_t kMSR_CORE_ENERGY_STAT = 0xC001029A;
    static constexpr uint32_t kMSR_HARDWARE_PSTATE_STATUS = 0xC0010293;
    static constexpr uint32_t kMSR_PKG_ENERGY_STAT = 0xC001029B;
    static constexpr uint32_t kMSR_PSTATE_0 = 0xC0010064;
    static constexpr uint32_t kMSR_PSTATE_LEN = 8;
    static constexpr uint32_t kMSR_PSTATE_STAT = 0xC0010063;
    static constexpr uint32_t kMSR_PSTATE_CTL = 0xC0010062;
    static constexpr uint32_t kMSR_RAPL_PWR_UNIT = 0xC0010299;
    static constexpr uint32_t kMSR_MPERF = 0x000000E7;
    static constexpr uint32_t kMSR_APERF = 0x000000E8;
    static constexpr uint32_t kMSR_PERF_CTL_0 = 0xC0010000;
    static constexpr uint32_t kMSR_PERF_CTR_0 = 0xC0010004;
    static constexpr uint32_t kMSR_PERF_IRPC = 0xC00000E9;
    static constexpr uint32_t kMSR_CSTATE_ADDR = 0xC0010073;
    
    
//    static constexpr uint32_t EF = 0x88;
    
    static constexpr uint32_t kEFI_VARIABLE_NON_VOLATILE = 0x00000001;
    static constexpr uint32_t kEFI_VARIABLE_BOOTSERVICE_ACCESS = 0x00000002;
    static constexpr uint32_t kEFI_VARIABLE_RUNTIME_ACCESS = 0x00000004;
    
    

    virtual bool init(OSDictionary *dictionary = 0) override;
    virtual void free(void) override;
    
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService* whatDevice) override;
    
    void fetchOEMBaseBoardInfo();

    bool read_msr(uint32_t addr, uint64_t *value);
    bool write_msr(uint32_t addr, uint64_t value);
    
    
    void calculateEffectiveFrequency(uint8_t physical);
    void updateInstructionDelta(uint8_t physical);
    void applyPowerControl(uint8_t pstate);
    uint8_t getPowerControl();

    /**
    /**
     *  0.7.6: single entry point for user alerts.
     *
     *  kunc_alert stays nullptr when _KUNCUserNotificationDisplayAlert cannot be
     *  resolved, and the user client used to dereference it unconditionally (once
     *  in hasPrivilege() and once in externalMethod()) - one user-space call was
     *  enough to crash the kernel.
     *
     *  The null check now happens in one place:
     *  - alert available: show it, the response is valid, return true;
     *  - alert unavailable: degrade to IOLog, set the response to UINT_MAX and
     *    return false ("the user was not asked" => the caller must not treat the
     *    result as consent).
     *
     *  0.7.6 (Round 2 F4): the sentinel is UINT_MAX rather than 0 - 0 is a valid
     *  button index (the CFUserNotification default button), so using it as "the
     *  user was not asked" would be misread as "the user pressed the first
     *  button".
     */
     */
    bool presentAlert(const char *title, const char *body,
                      const char *btn1, const char *btn2, const char *btn3,
                      unsigned int *response);
    
    bool setCPBState(bool enabled);
    bool getCPBState();
    
#define HF_TEMP_SAMPLE_SECS 3
#define HF_TEMP_SAMPLE_FREQ 2
#define HF_TEMP_SAMPLE_LEN (HF_TEMP_SAMPLE_SECS * HF_TEMP_SAMPLE_FREQ)
#define HF_TEMP_SAMPLE_LENREP (1.0f / (float)HF_TEMP_SAMPLE_LEN)
#define HF_TEMP_SAMPLE_REP (1.0f / (float)HF_TEMP_SAMPLE_FREQ)
#define HF_TEMP_SAMPLE_PERIOD (int)(HF_TEMP_SAMPLE_REP * 1000.0)
    inline float getPackageTemp();
    void updatePackageTemp();
    
    void updatePackageEnergy();
    
    void registerRequest();
    
    bool dumpPstate(uint64_t buf[8], bool allowLog = true);
    void writePstate(const uint64_t *buf);
    void restoreWakeState();
    
    bool initSuperIO(uint16_t* chipIntel);
    
    uint32_t getPMPStateLimit();
    void setPMPStateLimit(uint32_t);
    
    uint32_t getHPcpus();
    
    uint32_t totalNumberOfPhysicalCores;
    uint32_t totalNumberOfLogicalCores;
    
    uint8_t cpuFamily;
    uint8_t cpuModel;
    uint8_t cpuSupportedByCurrentVersion;
    
    //Cache size in KB
    uint32_t cpuCacheL1_perCore;
    uint32_t cpuCacheL2_perCore;
    uint32_t cpuCacheL3;
    
    char boardVender[BASEBOARD_STRING_MAX]{};
    char boardName[BASEBOARD_STRING_MAX]{};
    bool boardInfoValid = false;
    
    
    /**
     *  Hard allocate space for cached readings.
     */
    float effFreq_perCore[CPUInfo::MaxCpus] {};
    float PACKAGE_TEMPERATURE_perPackage[CPUInfo::MaxCpus];
    
    uint64_t lastMPERF_perCore[CPUInfo::MaxCpus];
    uint64_t lastAPERF_perCore[CPUInfo::MaxCpus];
    uint64_t deltaMPERF_perCore[CPUInfo::MaxCpus];
    
//    uint64_t lastAPERF_PerCore[CPUInfo::MaxCpus];
    
    uint64_t instructionDelta_perCore[CPUInfo::MaxCpus];
    uint64_t lastInstructionDelta_perCore[CPUInfo::MaxCpus];
    
    float loadIndex_perCore[CPUInfo::MaxCpus];
    
    float PStateStepUpRatio = 0.36;
    float PStateStepDownRatio = 0.05;
    
    uint8_t PStateCur_perCore[CPUInfo::MaxCpus];
    uint8_t PStateCtl = 0;
    // Zero-initialized: the wake path snapshots/restores these caches without
    // re-reading the MSRs, so an untouched slot must read as "no definition"
    // (0 fails the fid/dfsId validity check) rather than as kalloc garbage.
    uint64_t PStateDef_perCore[8] {};
    float PStateDefClock_perCore[8] {};
    bool cpbSupported;

    // Per-CPU flag: the HWCR init (setting bit 30) failed on this CPU and has
    // to be retried. Each CPU only touches its own slot, so no cross-CPU race.
    bool hwcrInitPending[CPUInfo::MaxCpus] {};

    // Per-CPU retry budget for the HWCR init. 0.7.6: the retry used to be
    // unbounded — a permanently unreadable HWCR meant one read + one write
    // inside the no-interrupt barrier on every single tick, forever. The
    // budget is refilled on each wake re-init. Each CPU only touches its own
    // slot, so no cross-CPU race.
    uint8_t hwcrInitRetryLeft[CPUInfo::MaxCpus] {};

    // 0.7.6: set when the cached P-state table could not be (re)read AND the
    // bounded retry budget ran out. The work loop reports it OUTSIDE the
    // rendezvous barrier (IOLog inside a no-interrupt barrier is exactly what
    // the 2026-08-26 panic taught us not to do).
    bool pstateDumpRetryExhausted {};

    // Set when the cached P-state table could not be (re)read; the work loop
    // retry tick re-runs dumpPstate() on the master CPU until it succeeds.
    bool pstateDumpPending {};

    // Bounded retry budget for the pstateDumpPending re-dump: every rdmsr
    // takes cpu_lock inside the rendezvous barrier, so a permanently
    // unreadable P-state MSR must not retry every tick forever. Refilled on
    // each wake re-init and on every explicit user writePstate().
    uint8_t pstateDumpRetryLeft { 20 };

    // Set once lastAPERF/lastMPERF hold a real sample, so a failed boot-time
    // seed does not yield one frequency sample scaled from cumulative counters.
    bool freqCountersSeeded[CPUInfo::MaxCpus] {};
    
    
    uint64_t lastUpdateTime;
    uint64_t lastUpdateEnergyValue;
    
    double uniPackageEnergy;
    
    bool disablePrivilegeCheck = false;
    uint16_t savedSMCChipIntel = 0;
    uint16_t kextloadAlerts = 0;

    kern_return_t (*kunc_alert)(int,unsigned,const char*,const char*,const char*,
                                const char*,const char*,const char*,const char*,const char*,unsigned*) {nullptr};
    
    
    ISSuperIOSMCFamily *superIO{nullptr};
    
private:
    IOWorkLoop *workLoop;
    IOTimerEventSource *timerEvent_main;
    IOTimerEventSource *timerEvent_tempe;
    
    bool serviceInitialized = false;
    
    uint32_t updateTimeInterval = 1000;
    uint32_t actualUpdateTimeInterval = 1;
    uint32_t timeOfLastUpdate = 0;
    uint32_t estimatedRequestTimeInterval = 0;
    uint32_t timeOfLastMissedRequest = 0;
    
    int tempNextSample = 0;
    float tempSamples[HF_TEMP_SAMPLE_LEN];
    float tempOffset = 0;
    double pwrTimeUnit = 0;
    double pwrEnergyUnit = 0;
    uint64_t pwrLastTSC = 0;
    // 0.7.6 (Round 2 P2): when the RAPL unit register cannot be read (the old code
    // panicked here) power measurement is disabled entirely instead of taking the
    // machine down.
    bool raplValid = false;
    // 0.7.6 (T8 / O24): after sleep the RAPL counter may restart from 0, so
    // subtracting the pre-sleep baseline would give a huge delta => a huge spike in
    // the first power reading after wake. Raised in the wake branch, where the work
    // loop is still disabled; the first sample after wake re-establishes the
    // baseline.
    bool pwrNeedRebaseline = false;
    // 0.7.6 (T8 / O12): throttle timestamp (ns) for the dual-value comparison log.
    // The main work-loop timer can fire every 50 ms and printing on every sample
    // would flood the log; once per 30 s is enough to see the trend.
    uint64_t lastPowerDiagNs = 0;
    
    uint64_t xnuTSCFreq = 1;
    // 0.7.6: _tscFreq is a pointer to a kernel variable. When the symbol is
    // missing, dereferencing it panicked the kernel at start(); the frequency
    // is now validated and the dependent power measurement is skipped.
    bool tscFreqValid = false;
    int (*wrmsr_carefully)(uint32_t, uint32_t, uint32_t) {nullptr};
    processor_t(*cpu_to_processor)(int);
    kern_return_t(*processor_shutdown)(processor_t);
    kern_return_t(*processor_startup)(processor_t);
    
    CPUInfo::CpuTopology cpuTopology {};
    
    IOPCIDevice *fIOPCIDevice;
    
    KernelPatcher *liluKernelPatcher;
    
    bool getPCIService();
    
    SleepState sleepState;
    
    void initWorkLoop();
    void stopWorkLoop();
    void resumeWorkLoop();
};
#endif
