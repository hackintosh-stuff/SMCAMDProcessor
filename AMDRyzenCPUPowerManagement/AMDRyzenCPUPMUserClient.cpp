//
//  AMDRyzenCPUPowerManagementUserClient.cpp
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 2/4/20.
//

#include "AMDRyzenCPUPMUserClient.hpp"



OSDefineMetaClassAndStructors(AMDRyzenCPUPMUserClient, IOUserClient);

// 0.7.6 (O31): output buffer size validation.
//
// On entry to externalMethod(), IOExternalMethodArguments::structureOutputSize is
// the number of bytes *provided by user space* (the MIG external method
// convention); on return it has to be set to the number of bytes actually
// produced. The old code treated "how much I want to write" as "how big the
// buffer is", so a user-space caller could pass a smaller buffer and make the
// kernel write past the end of the heap - a user-space controllable kernel heap
// overflow.
//
// Everything is checked here: if the buffer is too small, return
// kIOReturnBadArgument and write nothing at all. (IOExternalMethodDispatch in the
// SDK is the old structure and has no kernel-side checkStructureOutputSize
// helper, so the same check is implemented here.)
static inline bool checkOutputSize(IOExternalMethodArguments *arguments, uint32_t requiredBytes){
    if(arguments == NULL || arguments->structureOutput == NULL) return false;
    if(arguments->structureOutputSize < requiredBytes) return false;
    arguments->structureOutputSize = requiredBytes;
    return true;
}

// 0.7.6 (same class as O31): scalar outputs need validation too.
// On entry scalarOutputCount is the number of scalars (in UInt64 units) provided
// by user space, and scalarOutput is the user-provided buffer pointer. Writing
// through a NULL pointer is a kernel null pointer write - also a user-triggerable
// kernel panic.
static inline bool checkScalarOutput(IOExternalMethodArguments *arguments, uint32_t count){
    if(arguments == NULL) return false;
    if(count == 0) return true;
    if(arguments->scalarOutput == NULL) return false;
    if(arguments->scalarOutputCount < count) return false;
    arguments->scalarOutputCount = count;
    return true;
}



bool AMDRyzenCPUPMUserClient::initWithTask(task_t owningTask,
                                             void *securityToken,
                                             UInt32 type,
                                             OSDictionary *properties){
    
    if(!IOUserClient::initWithTask(owningTask, securityToken, type, properties)){
        return false;
    }
    
    token = securityToken;
    
    proc_t proc = (proc_t)get_bsdtask_info(owningTask);
    proc_name(proc_pid(proc), taskProcessBinaryName, 32);
    clientAuthorizedByUser = false;
    

    
    return true;

}

bool AMDRyzenCPUPMUserClient::start(IOService *provider){
    
    IOLog("AMDCPUSupportUserClient::start\n");
    
    bool success = IOService::start(provider);
    
    if(success){
        fProvider = OSDynamicCast(AMDRyzenCPUPowerManagement, provider);
    }
    
    return success;
}

void AMDRyzenCPUPMUserClient::stop(IOService *provider){
    IOLog("AMDCPUSupportUserClient::stop\n");
    
    fProvider = nullptr;
    IOService::stop(provider);
}

// Without this, every open/close (or process exit) leaked one user client
// instance until the user client slots were exhausted and IOServiceOpen failed.
IOReturn AMDRyzenCPUPMUserClient::clientClose(){
    // The default implementation is a no-op. terminate() releases the instance,
    // which is what actually frees the slot; isInactive() keeps this safe to
    // call when the provider is already being torn down.
    if(!isInactive())
        terminate();

    return kIOReturnSuccess;
}

//this is a meme, not getting the joke? nvm.
uint64_t multiply_two_numbers(uint64_t number_one, uint64_t number_two){
    uint64_t number_three = 0;
    for(uint32_t i = 0; i < number_two; i++){
        number_three = number_three + number_one;
    }
    return number_three;
}

bool AMDRyzenCPUPMUserClient::hasPrivilege(){
    if(fProvider->disablePrivilegeCheck) return true;
    if(clientHasPrivilege(token, kIOClientPrivilegeAdministrator) == kIOReturnSuccess) return true;
    if(clientAuthorizedByUser) return true;
    
    char buf[128];
    snprintf(buf, 128,
             "A process is trying to make changes to your system.\nAffected process name: %s\n\nAuthorize?",
             taskProcessBinaryName);
    
    unsigned int rf = 0;
    // 0.7.6: when the alert channel is unavailable (kunc_alert == nullptr) the user
    // cannot be asked => deny. This used to dereference the null pointer
    // unconditionally, so one user-space call could crash the kernel.
    if(!fProvider->presentAlert("AMDRyzenCPUPowerManagement", buf,
                                "Deny", "Until Process Terminate", "Once", &rf)){
        return false;
    }
    
    
    if(rf == 1){
        clientAuthorizedByUser = true;
        return true;
    }
    
    if(rf == 2){
        return true;
    }
    
    return false;
}

IOReturn AMDRyzenCPUPMUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                                 IOExternalMethodDispatch *dispatch,
                                                   OSObject *target, void *reference){
    
    if (fProvider->kextloadAlerts) {
        unsigned int rf = 0;
        
        char buf[128];
        snprintf(buf, 128,
                 "Kext alert detected: %d",
                 fProvider->kextloadAlerts);
        
        // 0.7.6: when the alert is unavailable, degrade to a log message (presentAlert()
        // already logs) and clear the counter so every call does not reprint it. When the
        // alert is available the original behaviour is kept.
        if(!fProvider->presentAlert("AMDRyzenCPUPowerManagement", buf,
                                    "Ok", "Ok and Clear Alert", "WTF?", &rf)){
            fProvider->kextloadAlerts = 0;
        } else if(rf == 1){
            fProvider->kextloadAlerts = 0;
        }
    }
    
    fProvider->registerRequest();
    
    switch (selector) {
            
        //Get PStateDef raw values for core 0
        case 0: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, (fProvider->kMSR_PSTATE_LEN) * sizeof(uint64_t))) return kIOReturnBadArgument;

            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            for(uint32_t i = 0; i < fProvider->kMSR_PSTATE_LEN; i++){
                dataOut[i] = fProvider->PStateDef_perCore[i];
            }
            
            break;
        }
            
            
        //Get PStateDef floating point clock values for core 0
        case 1: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, (fProvider->kMSR_PSTATE_LEN) * sizeof(float))) return kIOReturnBadArgument;

            float *dataOut = (float*) arguments->structureOutput;
            
            for(uint32_t i = 0; i < fProvider->kMSR_PSTATE_LEN; i++){
                dataOut[i] = fProvider->PStateDefClock_perCore[i];
            }
            
            break;
        }
            
        case 2: {
            
            uint32_t numPhyCores = fProvider->totalNumberOfPhysicalCores;

            if(!checkScalarOutput(arguments, 1)) return kIOReturnBadArgument;
            arguments->scalarOutput[0] = numPhyCores;
            
            if(!checkOutputSize(arguments, numPhyCores * sizeof(float))) return kIOReturnBadArgument;

            float *dataOut = (float*) arguments->structureOutput;

            for(uint32_t i = 0; i < numPhyCores; i++){
                dataOut[i] = fProvider->effFreq_perCore[i];
            }
            
            break;
        }
        
        case 3: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, 1 * sizeof(float))) return kIOReturnBadArgument;
            
            float *dataOut = (float*) arguments->structureOutput;
            dataOut[0] = fProvider->PACKAGE_TEMPERATURE_perPackage[0];
            break;
        }
        
        //Get all data like this: [power, temp, pstateCur, clock_core_1, 2, 3 .....]
        //Yes, i am too lazy to write a struct
        case 4: {
            uint32_t numPhyCores = fProvider->totalNumberOfPhysicalCores;
            if(!checkScalarOutput(arguments, 1)) return kIOReturnBadArgument;
            arguments->scalarOutput[0] = numPhyCores;
            
            if(!checkOutputSize(arguments, (numPhyCores + 3) * sizeof(float))) return kIOReturnBadArgument;

            float *dataOut = (float*) arguments->structureOutput;
            
            dataOut[0] = (float)fProvider->uniPackageEnergy;
            dataOut[1] = fProvider->PACKAGE_TEMPERATURE_perPackage[0];
            dataOut[2] = fProvider->getPowerControl();
            
            for(uint32_t i = 0; i < numPhyCores; i++){
                dataOut[i + 3] = fProvider->effFreq_perCore[i];
            }
            
            break;
        }
            
        //Get per core raw load index
        case 5: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;

            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            dataOut[0] = 0;
            
            for(uint32_t i = 0; i < fProvider->totalNumberOfLogicalCores; i++){
                dataOut[0] += fProvider->instructionDelta_perCore[i];
            }
            
            break;
        }
            
        //Get per core load index
        case 6: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, (fProvider->totalNumberOfPhysicalCores) * sizeof(float))) return kIOReturnBadArgument;

            float *dataOut = (float*) arguments->structureOutput;
            
            // 0.7.6: divide-by-zero guard. With 0 physical cores the integer division raises
            // #DE and crashes the kernel (one user-space call is enough).
            if(fProvider->totalNumberOfPhysicalCores == 0){
                arguments->scalarOutputCount = 0;
                arguments->structureOutputSize = 0;
                break;
            }
            
            int lcpu_percore = fProvider->totalNumberOfLogicalCores / fProvider->totalNumberOfPhysicalCores;
            
            for(uint32_t i = 0; i < fProvider->totalNumberOfPhysicalCores; i++){
                float l = pmRyzen_avgload_pcpu(i * lcpu_percore);
//                dataOut[i] = fProvider->loadIndex_PerCore[i];
                dataOut[i] = l;
            }
            
            break;
        }
            
        //Get basic CPUID
        //[Family, Model, Physical, Logical, L1_perCore, L2_perCore, L3]
        case 7: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, (8) * sizeof(uint64_t))) return kIOReturnBadArgument;

            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            dataOut[0] = (uint64_t)fProvider->cpuFamily;
            dataOut[1] = (uint64_t)fProvider->cpuModel;
            dataOut[2] = (uint64_t)fProvider->totalNumberOfPhysicalCores;
            dataOut[3] = (uint64_t)fProvider->totalNumberOfLogicalCores;
            dataOut[4] = (uint64_t)fProvider->cpuCacheL1_perCore;
            dataOut[5] = (uint64_t)fProvider->cpuCacheL2_perCore;
            dataOut[6] = (uint64_t)fProvider->cpuCacheL3;
            dataOut[7] = (uint64_t)fProvider->cpuSupportedByCurrentVersion;
            
            break;
        }
        
        //Get AMDRyzenCPUPowerManagement Version String
        case 8: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, sizeof(xStringify(MODULE_VERSION)))) return kIOReturnBadArgument;
            char *dataOut = (char*) arguments->structureOutput;
            
            for(uint32_t i = 0; i < arguments->structureOutputSize; i++){
                dataOut[i] = xStringify(MODULE_VERSION)[i];
            }
            
            break;
        }
        
        //Get PState
        case 9: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;
            
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;

            dataOut[0] = fProvider->PStateCtl;
            
            break;
        }
        
        //Set PState
        case 10: {
            arguments->scalarOutputCount = 0;
            arguments->structureOutputSize = 0;
            
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
            
            fProvider->PStateCtl = (uint8_t)arguments->scalarInput[0];
            fProvider->applyPowerControl(255);
            break;
        }
            
        //Get CPB
        case 11: {
            arguments->scalarOutputCount = 0;
            
            if(!checkOutputSize(arguments, 2 * sizeof(uint64_t))) return kIOReturnBadArgument;
            
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;

            dataOut[0] = (uint64_t)fProvider->cpbSupported;
            dataOut[1] = (uint64_t)fProvider->getCPBState();
            break;
        }
        
        //Set CPB
        case 12: {
            arguments->scalarOutputCount = 0;
            arguments->structureOutputSize = 0;
            
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
            
            if(!fProvider->cpbSupported)
                return kIOReturnNoDevice;
            
            // Report the failure back to the client instead of silently
            // pretending the change went through.
            if(!fProvider->setCPBState(arguments->scalarInput[0]==1?true:false))
                return kIOReturnError;
            
            break;
        }
            
        //Get PPM
        case 13: {
            arguments->scalarOutputCount = 0;
                
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;
                
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;

            dataOut[0] = (uint64_t)(fProvider->getPMPStateLimit() == 0 ? 0 : 1);
            break;
        }
            
        //Set PPM
        case 14: {
            arguments->scalarOutputCount = 0;
            arguments->structureOutputSize = 0;
                
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
                
            boolean_t enabled = arguments->scalarInput[0]==1?true:false;
            
            fProvider->setPMPStateLimit(enabled ? 1 : 0);
            
            break;
        }
            
        //Set PStateDef
        case 15: {
            if(!hasPrivilege())
                return kIOReturnNotPrivileged;
            
            if(arguments->scalarInputCount != 8)
                return kIOReturnBadArgument;
            
            
            fProvider->writePstate(arguments->scalarInput);
            
            break;
        }
            
        //get board info
        case 16: {
            //Let's give that one more try :)
            if(!fProvider->boardInfoValid)
                fProvider->fetchOEMBaseBoardInfo();
            
            if(!checkScalarOutput(arguments, 1)) return kIOReturnBadArgument;
            arguments->scalarOutput[0] = fProvider->boardInfoValid ? 1 : 0;
            
            if(!checkOutputSize(arguments, 128)) return kIOReturnBadArgument;

            char *dataOut = (char*) arguments->structureOutput;
            
            for(uint32_t i = 0; i < 64; i++){
                dataOut[i] = fProvider->boardVender[i];
            }
            
            for(uint32_t i = 0; i < 64; i++){
                dataOut[i+64] = fProvider->boardName[i];
            }
            
            break;
        }
            
        case 17: {
            arguments->scalarOutputCount = 0;
                
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;
                
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;

            dataOut[0] = (uint64_t)(fProvider->getHPcpus());
            break;
        }
        
        //Get LPM
        case 18: {
            arguments->scalarOutputCount = 0;
                
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;
                
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;

            dataOut[0] = (uint64_t)(fProvider->getPMPStateLimit() == 2 ? 1 : 0);
            break;
        }
            
        //Set LPM
        case 19: {
            arguments->scalarOutputCount = 0;
            arguments->structureOutputSize = 0;
                
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
                
            boolean_t enabled = arguments->scalarInput[0]==1?true:false;
            
            fProvider->setPMPStateLimit(enabled ? 2 : 1);
            
            break;
        }
        
        //Try load SMC driver
        case 90: {
            
            arguments->scalarOutputCount = 0;
            if(!checkOutputSize(arguments, 2 * sizeof(uint64_t))) return kIOReturnBadArgument;
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            if(fProvider->superIO != nullptr){
                dataOut[0] = (uint64_t)(1);
                dataOut[1] = (uint64_t)(fProvider->savedSMCChipIntel);
                break;
            }
            
            uint16_t ci = 0;
            bool found = fProvider->initSuperIO(&ci);
            
            dataOut[0] = (uint64_t)(found ? 1 : 0);
            dataOut[1] = (uint64_t)(ci);
            break;
        }
        
        //SMC load number of fans
        case 91: {
            
            if(!fProvider->superIO)
                return kIOReturnNoDevice;

            
            arguments->scalarOutputCount = 0;
            if(!checkOutputSize(arguments, 1 * sizeof(uint64_t))) return kIOReturnBadArgument;
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            dataOut[0] = (uint64_t)(fProvider->superIO->getNumberOfFans());
            break;
        }
        
        //SMC load readable desc for fan
        case 92: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;

            arguments->scalarOutputCount = 0;
                
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
                
            // 0.7.6 (Round 3 leftover, same class as O31): the fan index is supplied by user
            // space and must be range-checked before it is used as an index. The original
            // implementation passed it straight to getReadableStringForFan(), whose bounds
            // check was `fan > activeFansOnSystem` (off by one), so:
            //   - index == fan count => kFAN_READABLE_STRS[fan] reads out of bounds =>
            //     a wild pointer from the neighbouring memory => strlen(wild pointer) =>
            //     kernel panic;
            //   - index > fan count  => nullptr is returned => strlen(nullptr) => kernel
            //     panic;
            //   - negative index (scalarInput is uint64, converted to int it can go negative)
            //     => out-of-bounds read.
            // All three are user-triggerable kernel panics.
            int fanIndex = (int)arguments->scalarInput[0];
            if(fanIndex < 0 || fanIndex >= fProvider->superIO->getNumberOfFans())
                return kIOReturnBadArgument;
            
            const char *str = fProvider->superIO->getReadableStringForFan(fanIndex);
            // getReadableStringForFan() returns nullptr for an unknown index and
            // strlen(nullptr) panics, so this second guard is required.
            if(str == NULL)
                return kIOReturnBadArgument;
            
            // 0.7.6 (O31): copy the NUL terminator as well. The old strcpy copied only
            // strlen bytes without the NUL, so a caller reading it as a C string would read
            // past the buffer. The required capacity is therefore strlen+1.
            uint32_t strLen = (uint32_t)strlen(str);
            if(!checkOutputSize(arguments, strLen + 1)) return kIOReturnBadArgument;
            
            char *dataOut = (char*) arguments->structureOutput;
            strcpy(dataOut, str, strLen + 1);
            
            
            break;
        }
            
        //SMC fan rpms
        case 93: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;
            
            arguments->scalarOutputCount = 0;
            if(!checkOutputSize(arguments, fProvider->superIO->getNumberOfFans() * sizeof(uint64_t))) return kIOReturnBadArgument;
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            fProvider->superIO->updateFanRPMS();
            for (int i = 0; i < fProvider->superIO->getNumberOfFans(); i++) {
                dataOut[i] = fProvider->superIO->getRPMForFan(i);
            }
            
            break;
        }
            
        default: {
            IOLog("AMDCPUSupportUserClient::externalMethod: invalid method.\n");
            break;
        }
        
        //SMC fan throttles and control mode
        case 94: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;
            
            arguments->scalarOutputCount = 0;
            if(!checkOutputSize(arguments, fProvider->superIO->getNumberOfFans() * sizeof(uint64_t))) return kIOReturnBadArgument;
            uint64_t *dataOut = (uint64_t*) arguments->structureOutput;
            
            fProvider->superIO->updateFanControl();
            for (int i = 0; i < fProvider->superIO->getNumberOfFans(); i++) {
                dataOut[i] = fProvider->superIO->getFanThrottle(i) << 8 | (fProvider->superIO->getFanAutoControlMode(i) ? 1 : 0);
            }
            
            break;
        }
        
        //SMC fan overrride control
        case 95: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;
            
            if(!hasPrivilege())
                return kIOReturnNotPrivileged;
            
            if(arguments->scalarInputCount != 2)
                return kIOReturnBadArgument;
            
            int fanSel = (int)arguments->scalarInput[0];
            uint8_t pwm = (uint8_t)arguments->scalarInput[1];
            
            fProvider->superIO->overrideFanControl(fanSel, pwm);
            
            break;
        }
        
        //SMC fan default control
        case 96: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;
            
            if(!hasPrivilege())
                return kIOReturnNotPrivileged;
            
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
            
            int fanSel = (int)arguments->scalarInput[0];
            
            fProvider->superIO->setDefaultFanControl(fanSel);
            
            break;
        }
        
        //SMC Secret Undocumented feature (⁎⁍̴̛ᴗ⁍̴̛⁎)
        case 97: {
            if(!fProvider->superIO)
                return kIOReturnNoDevice;
            
            if(!hasPrivilege())
                return kIOReturnNotPrivileged;
            
            if(arguments->scalarInputCount != 1)
                return kIOReturnBadArgument;
            
            int numFan = fProvider->superIO->getNumberOfFans();
            for (int i = 0; i < numFan; i++) {
                if(arguments->scalarInput[0])
                    fProvider->superIO->overrideFanControl(i, 0xff);
                else
                    fProvider->superIO->setDefaultFanControl(i);
            }
            
            break;
        }
    }
    
    return kIOReturnSuccess;
}
