/*
 * smcamd-test.c - user client smoke test for AMDRyzenCPUPowerManagement.
 *
 * Calls the same user client selectors AMD Power Gadget uses, so the kext can be
 * checked on a running system without Xcode and without the app. Everything it
 * does is read-only: it never writes an MSR, never changes the P-state and never
 * touches the SMC key store.
 *
 * Selectors exercised (see AMDRyzenCPUPMUserClient::externalMethod):
 *   8 -> kext version string as the app reads it
 *   0 -> raw P-state definitions (fid/dfs/enable decoding is done here)
 *   4 -> [package power W, package temp C, P-state ctl, per-core MHz ...]
 *   3 -> package temperature on its own
 *
 * Build and run on the target Mac (Command Line Tools are enough):
 *   clang -framework IOKit -framework CoreFoundation -o smcamd-test smcamd-test.c
 *   ./smcamd-test
 *
 * Exit status is 0 only when every selector returned KERN_SUCCESS, so the tool is
 * usable in a script. Run it as the logged-in user; the read-only selectors need
 * no root and do not raise the authorization prompt.
 */

#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

#define SMCAMD_SERVICE "AMDRyzenCPUPowerManagement"

/* Highest number of logical cores the kext can report (CPUInfo::MaxCpus). */
#define SMCAMD_MAX_CORES 64

static io_connect_t open_client(void) {
    /* kIOMainPortDefault exists since macOS 12; older SDKs only have the now
       deprecated kIOMasterPortDefault. Both are constants, so this has to be a
       version check rather than #ifdef. */
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                   IOServiceMatching(SMCAMD_SERVICE));
#else
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                   IOServiceMatching(SMCAMD_SERVICE));
#endif
    if (!svc) {
        printf("%s not found in the IORegistry (kext not loaded?)\n", SMCAMD_SERVICE);
        return 0;
    }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        printf("IOServiceOpen failed: 0x%08x\n", kr);
        return 0;
    }
    return conn;
}

int main(void) {
    io_connect_t conn = open_client();
    if (!conn)
        return 2;

    int failures = 0;
    kern_return_t kr;

    /* selector 8: version string, read exactly like the app does */
    {
        char buf[64];
        size_t size = sizeof(buf);
        memset(buf, 0, sizeof(buf));

        kr = IOConnectCallStructMethod(conn, 8, NULL, 0, buf, &size);
        printf("selector 8 (version)      : kr=0x%08x size=%zu \"%s\"\n", kr, size, buf);
        if (kr != KERN_SUCCESS)
            failures++;
    }

    /* selector 0: raw P-state definitions. A slot with fid == 0 or dfs == 0 is
       not populated; the clock is fid / dfs * 200 MHz. */
    {
        uint64_t defs[SMCAMD_MAX_CORES / 8];
        size_t size = sizeof(defs);
        memset(defs, 0, sizeof(defs));

        kr = IOConnectCallStructMethod(conn, 0, NULL, 0, defs, &size);
        printf("selector 0 (P-state defs) : kr=0x%08x\n", kr);
        if (kr != KERN_SUCCESS) {
            failures++;
        } else {
            for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
                uint64_t fid = defs[i] & 0xff;
                uint64_t dfs = (defs[i] >> 8) & 0x3f;
                int enabled = (int)((defs[i] >> 63) & 1);

                if (!fid || !dfs)
                    continue;
                /* clock = fid / dfs * 200 MHz, computed as fid * 200 / dfs so the
                   integer division does not truncate (144 / 10 * 200 = 2800,
                   144 * 200 / 10 = 2880). */
                printf("  P%zu: raw=0x%016llx en=%d fid=%llu dfs=%llu -> %llu MHz\n",
                       i, (unsigned long long)defs[i], enabled,
                       (unsigned long long)fid, (unsigned long long)dfs,
                       (unsigned long long)(fid * 200 / dfs));
            }
        }
    }

    /* selector 4: everything the app polls for its graphs */
    {
        uint64_t scalar[8];
        uint32_t scalarOut = 8;
        float buf[SMCAMD_MAX_CORES + 3];
        size_t size = sizeof(buf);
        memset(scalar, 0, sizeof(scalar));
        memset(buf, 0, sizeof(buf));

        kr = IOConnectCallMethod(conn, 4, NULL, 0, NULL, 0,
                                 scalar, &scalarOut, buf, &size);
        printf("selector 4 (all readings): kr=0x%08x cores=%llu\n",
               kr, (unsigned long long)scalar[0]);
        if (kr != KERN_SUCCESS || scalar[0] > SMCAMD_MAX_CORES) {
            failures++;
        } else {
            printf("  package power : %.2f W\n", buf[0]);
            printf("  package temp  : %.2f C\n", buf[1]);
            printf("  P-state ctl   : %u\n", (unsigned)buf[2]);
            for (uint64_t i = 0; i < scalar[0]; i++)
                printf("  core %-2llu     : %.1f MHz\n", (unsigned long long)i, buf[i + 3]);
        }
    }

    /* selector 3: package temperature, the value the SMC keys report */
    {
        float temp = 0;
        size_t size = sizeof(temp);

        kr = IOConnectCallStructMethod(conn, 3, NULL, 0, &temp, &size);
        printf("selector 3 (package temp) : kr=0x%08x %.2f C\n", kr, temp);
        if (kr != KERN_SUCCESS)
            failures++;
    }

    IOServiceClose(conn);

    if (failures) {
        printf("%d selector(s) failed\n", failures);
        return 1;
    }
    return 0;
}
