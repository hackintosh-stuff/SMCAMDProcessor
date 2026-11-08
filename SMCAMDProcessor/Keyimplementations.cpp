//
//  KeyImplementations.cpp
//  AMDRyzenCPUPowerManagement
//
//  Created by trulyspinach on 2/12/20.
//

#include "KeyImplementations.hpp"


// Sensor keys are read from arbitrary user-space threads, possibly before the
// driver published its first sample. A zero or out-of-range value would be shown
// as 0 C (or a negative temperature) by sensor applications, so clamp it to a
// neutral reading instead.
static inline double sanitizeTemp(double temp) {
    if (temp <= 0.0 || temp > 125.0) return 45.0;
    return temp;
}

SMC_RESULT TempPackage::readAccess() {
    uint16_t *ptr = reinterpret_cast<uint16_t *>(data);
    *ptr = VirtualSMCAPI::encodeSp(type, sanitizeTemp((double)provider->PACKAGE_TEMPERATURE_perPackage[0]));

    return SmcSuccess;
}

SMC_RESULT TempCore::readAccess() {
    uint16_t *ptr = reinterpret_cast<uint16_t *>(data);
    *ptr = VirtualSMCAPI::encodeSp(type, sanitizeTemp((double)provider->PACKAGE_TEMPERATURE_perPackage[0]));

    return SmcSuccess;
}

SMC_RESULT ClockCore::readAccess() {
    uint16_t *ptr = reinterpret_cast<uint16_t *>(data);
    float clock = provider->effFreq_perCore[core] / 100;
    *ptr = VirtualSMCAPI::encodeSp(type, (double)clock);

    return SmcSuccess;
}

SMC_RESULT EnergyPackage::readAccess(){
    // Same reasoning as above for power: a bogus sample (unusable RAPL unit,
    // sample skipped before the first baseline) must not surface as a negative
    // or multi-kilowatt reading.
    double energy = provider ? provider->uniPackageEnergy : 0.0;
    if (energy < 0.0 || energy > 500.0) energy = 0.0;

    if (type == SmcKeyTypeFloat)
        *reinterpret_cast<uint32_t *>(data) = VirtualSMCAPI::encodeFlt(energy);
    else
        *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeSp(type, energy);

    return SmcSuccess;
}
