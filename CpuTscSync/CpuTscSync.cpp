//
//  CpuTscSync.cpp
//  CpuTscSync
//
//  Copyright © 2021 lvs1974. All rights reserved.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_cpu.hpp>
#include <i386/proc_reg.h>
#include <IOKit/IOTimerEventSource.h>


#include "CpuTscSync.hpp"

static CpuTscSyncPlugin *callbackCpuf = nullptr;
_Atomic(bool) CpuTscSyncPlugin::tsc_synced = true;
_Atomic(bool) CpuTscSyncPlugin::kernel_is_awake = false;


//stamp the tsc
void CpuTscSyncPlugin::stamp_tsc(void *tscp)
{
    wrmsr64(MSR_IA32_TSC, *reinterpret_cast<uint64_t*>(tscp));
}


void CpuTscSyncPlugin::tsc_adjust_or_reset()
{

    uint64_t tsc = rdtsc64();
    DBGLOG("cputs", "current tsc from rdtsc64() is %lld. Rendezvouing..", tsc);
    // call the kernel function that will call this "action" on all cores/processors
    mp_rendezvous_no_intrs(stamp_tsc, &tsc);
    
    tsc_synced = true;
}

void CpuTscSyncPlugin::init()
{
    callbackCpuf = this;

    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) {
            static_cast<CpuTscSyncPlugin *>(user)->processKernel(patcher);
    }, this);
}


IOReturn CpuTscSyncPlugin::IOHibernateSystemHasSlept()
{
    DBGLOG("cputs", "IOHibernateSystemHasSlept is called");
    tsc_synced = false;
    kernel_is_awake = false;
    return FunctionCast(IOHibernateSystemHasSlept, callbackCpuf->orgIOHibernateSystemHasSlept)();
}

IOReturn CpuTscSyncPlugin::IOHibernateSystemWake()
{
    IOReturn result = FunctionCast(IOHibernateSystemWake, callbackCpuf->orgIOHibernateSystemWake)();
    kernel_is_awake = true;
    return result;
}

void CpuTscSyncPlugin::IOPMrootDomain_tracePoint( void *that, uint8_t point )
{

    FunctionCast(IOPMrootDomain_tracePoint, callbackCpuf->orgIOPMrootDomain_tracePoint)(that, point);
    
    if(point == kIOPMTracePointWakeCPUs){
        kernel_is_awake = true;
        tsc_synced = false;
    }
}

void CpuTscSyncPlugin::clock_get_calendar_microtime(clock_sec_t *secs, clock_usec_t *microsecs)
{
    FunctionCast(clock_get_calendar_microtime, callbackCpuf->org_clock_get_calendar_microtime)(secs, microsecs);
  
    if (!tsc_synced && kernel_is_awake) {
        DBGLOG("cputs", "clock_get_calendar_microtime is called after wake");
        tsc_adjust_or_reset();
    }
}

void CpuTscSyncPlugin::processKernel(KernelPatcher &patcher)
{
    if (!kernel_routed)
    {
        KernelPatcher::RouteRequest requests_for_long_jump[] {
            {"_IOHibernateSystemHasSlept", IOHibernateSystemHasSlept, orgIOHibernateSystemHasSlept},
            {"_IOHibernateSystemWake", IOHibernateSystemWake, orgIOHibernateSystemWake}
        };
        
        size_t size = arrsize(requests_for_long_jump);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, requests_for_long_jump, size))
            SYSLOG("cputs", "patcher.routeMultiple for %s is failed with error %d", requests_for_long_jump[0].symbol, patcher.getError());
        
        patcher.clearError();
        
        KernelPatcher::RouteRequest requests[] {
            {"__ZN14IOPMrootDomain10tracePointEh", IOPMrootDomain_tracePoint, orgIOPMrootDomain_tracePoint},
            {"_clock_get_calendar_microtime", clock_get_calendar_microtime, org_clock_get_calendar_microtime }
        };

        size = arrsize(requests);
        if (!patcher.routeMultiple(KernelPatcher::KernelID, requests, size))
            SYSLOG("cputs", "patcher.routeMultiple for %s is failed with error %d", requests[0].symbol, patcher.getError());

        patcher.clearError();

        kernel_routed = true;
    }

    // Ignore all the errors for other processors
    patcher.clearError();
}
