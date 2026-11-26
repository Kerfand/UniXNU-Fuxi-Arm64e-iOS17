/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8550GeniUart.h"
#include "Sm8550QupDma.h"
#include <IOKit/IOLocks.h>
static IOLock *gcc_lock;
static IOLock *smmu_lock;
static uint32_t uart14_initial_bus;
static bool uart14_claimed;
static IOLock *get_gcc_lock(void) {
    IOLock *lock=__atomic_load_n(&gcc_lock,__ATOMIC_ACQUIRE);
    if(!lock) {
        IOLock *candidate=IOLockAlloc();if(!candidate)return NULL;
        if(__atomic_compare_exchange_n(&gcc_lock,&lock,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))lock=candidate;
        else IOLockFree(candidate);
    }
    return lock;
}
bool sm8550_apps_smmu_lock(void) {
    IOLock *lock=__atomic_load_n(&smmu_lock,__ATOMIC_ACQUIRE);
    if(!lock) {
        IOLock *candidate=IOLockAlloc();if(!candidate)return false;
        if(__atomic_compare_exchange_n(&smmu_lock,&lock,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))lock=candidate;
        else IOLockFree(candidate);
    }
    IOLockLock(lock);return true;
}
void sm8550_apps_smmu_unlock(void) {IOLockUnlock(smmu_lock);}
bool sm8550_gcc_update32(volatile uint32_t *reg,uint32_t clear,uint32_t set) {
    if(!reg)return false;
    IOLock *lock=get_gcc_lock();if(!lock)return false;
    IOLockLock(lock);*reg=(*reg&~clear)|set;
    __asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&set)==set && (*reg&(clear&~set))==0;
    IOLockUnlock(lock);return ok;
}
bool sm8550_geni_uart14_vote(volatile uint32_t *reg,bool enabled) {
    if(!reg)return false;
    IOLock *lock=get_gcc_lock();if(!lock)return false;
    IOLockLock(lock);uint32_t value=*reg;
    if(!enabled && !uart14_claimed){IOLockUnlock(lock);return true;}
    if(enabled) {
        if(!uart14_claimed){uart14_initial_bus=value&0xfu;uart14_claimed=true;}
        value|=0x40fu;
    } else {
        value&=~0x400u;
        if(!(value&0x203f0u))value&=~(0xfu&~uart14_initial_bus);
    }
    *reg=value;__asm__ volatile("dsb sy" ::: "memory");
    bool ok=(*reg&0x40fu)==(value&0x40fu);IOLockUnlock(lock);return ok;
}
