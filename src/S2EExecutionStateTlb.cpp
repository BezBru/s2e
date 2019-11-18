///
/// Copyright (C) 2012-2014, Dependable Systems Laboratory, EPFL
/// Copyright (C) 2014-2016, Cyberhaven
/// All rights reserved.
///
/// Licensed under the Cyberhaven Research License Agreement.
///

#include <klee/AddressSpace.h>
#include <llvm/Support/CommandLine.h>
#include <s2e/cpu.h>
#include <s2e/s2e_libcpu.h>

#include <s2e/S2EExecutionStateTlb.h>
#include <s2e/Utils.h>
#include <s2e/s2e_config.h>

//#define S2E_DEBUG_TLBCACHE

#ifdef S2E_DEBUG_TLBCACHE
#include <s2e/S2E.h>
#else
// Undefine cat from "compiler.h"
#undef cat
#endif

#include <klee/AddressSpace.h>
#include <llvm/Support/CommandLine.h>

extern llvm::cl::opt<bool> PrintModeSwitch;

namespace s2e {

using namespace klee;

void S2EExecutionStateTlb::addressSpaceChangeUpdateTlb(const klee::ObjectStateConstPtr &oldState,
                                                       const klee::ObjectStatePtr &newState) {
    if (!(oldState && oldState->isMemoryPage())) {
        return;
    }

    /* This can happen when KLEE unbinds the object after it was
     * split into multiple ones. */
    if (!newState) {
        return;
    }

    assert(oldState->getStoreOffset() == 0);

    updateTlb(oldState, newState);
}

#if defined(SE_ENABLE_PHYSRAM_TLB)
void S2EExecutionStateTlb::updateRamTlb(const klee::ObjectStateConstPtr &oldState,
                                        const klee::ObjectStatePtr &newState) {
    CPUX86State *cpu = m_registers->getCpuState();
    uintptr_t tlb_index = (oldState->getAddress() >> 12) & (CPU_TLB_SIZE - 1);
    CPUTLBRAMEntry *re = &cpu->se_ram_tlb[tlb_index];

    assert(oldState->isSharedConcrete() == newState->isSharedConcrete());

    auto address = oldState->getAddress();

    if (!oldState->isSharedConcrete() && re->object_state == oldState) {
        assert((re->host_page & ~TLB_NOT_OURS) == address);
        if (newState->isAllConcrete()) {
            // XXX: use proper ref counting
            re->object_state = newState.get();
            re->host_page = address;
            re->addend = (uintptr_t) newState->getConcreteBuffer()->get() - address;
        }
        if (m_asCache->isOwnedByUs(newState)) {
            re->host_page &= ~TLB_NOT_OURS;
        }
    }
}
#endif

void S2EExecutionStateTlb::updateTlb(const klee::ObjectStateConstPtr &oldState, const klee::ObjectStatePtr &newState) {

    CPUX86State *cpu = m_registers->getCpuState();

    if (g_s2e_single_path_mode) {
        llvm::errs() << "Multi-path mode disabled.\n";
        exit(-1);
    }

#ifdef S2E_DEBUG_TLBCACHE
    g_s2e->getDebugStream(g_s2e_state) << "addressSpaceChangeUpdateTlb: Replacing " << hexval(oldState) << " by "
                                       << hexval(newState) << "\n";
    g_s2e->getDebugStream(g_s2e_state) << "addressSpaceChangeUpdateTlb: tlb map size=" << m_tlbMap.size() << '\n';
#endif

    assert(oldState->isSharedConcrete() == newState->isSharedConcrete());

    auto it = m_tlbMap.find(oldState);
    bool found = false;
    if (it != m_tlbMap.end()) {
        found = true;
        ObjectStateTlbReferences vec = (*it).second;
        unsigned size = vec.size();
        assert(size > 0);
        for (unsigned i = 0; i < size; ++i) {
            const TlbCoordinates &coords = vec[i];
#ifdef S2E_DEBUG_TLBCACHE
            g_s2e->getDebugStream() << "    mmu_idx=" << coords.first << " index=" << coords.second << "\n";
#endif
            CPUTLBEntry *entry = &cpu->tlb_table[coords.first][coords.second];
            assert(entry->objectState == (void *) oldState.get());
            assert(newState);

            // XXX: use proper refcounting
            entry->objectState = newState.get();

            if (!newState->isSharedConcrete()) {
                // The addend does not change.
                entry->se_addend = entry->se_addend - (uintptr_t) oldState->getConcreteStore(true) +
                                   (uintptr_t) newState->getConcreteStore(true);

                if (m_asCache->isOwnedByUs(newState)) {
                    entry->addr_write &= ~TLB_NOT_OURS;
                }
            }

            updateTlbEntryConcreteStatus(cpu, coords.first, coords.second, newState);
        }

        m_tlbMap[newState] = vec;
        if (newState != oldState) {
            m_tlbMap.erase(oldState);
        }
    }

#ifdef S2E_DEBUG_TLBCACHE
    audit();
#endif
}

/***/

void S2EExecutionStateTlb::flushTlbCache() {
#ifdef S2E_DEBUG_TLBCACHE
    g_s2e->getDebugStream(g_s2e_state) << "Flushing TLB cache\n";
#endif
    m_tlbMap.clear();
}

void S2EExecutionStateTlb::flushTlbCachePage(const klee::ObjectStatePtr &objectState, int mmu_idx, int index) {
    if (!objectState) {
        return;
    }

#ifdef S2E_DEBUG_TLBCACHE
    g_s2e->getDebugStream(g_s2e_state) << "flushTlbCachePage: clearing cache entry for " << objectState << " ("
                                       << mmu_idx << "," << index << ")\n";
#endif

    env->tlb_table[mmu_idx][index].objectState = 0;
    env->tlb_table[mmu_idx][index].se_addend = 0;

    if (g_s2e_single_path_mode) {
        return;
    }

    bool found = false;
    TlbMap::iterator tlbIt = m_tlbMap.find(objectState);
    assert(tlbIt != m_tlbMap.end());

    ObjectStateTlbReferences &vec = (*tlbIt).second;
    foreach2 (vit, vec.begin(), vec.end()) {
        if ((*vit) == std::make_pair((unsigned) mmu_idx, (unsigned) index)) {
            vec.erase(vit);
            found = true;
            break;
        }
    }

    assert(found && "Invalid cache!");

    if (vec.empty()) {
#ifdef S2E_DEBUG_TLBCACHE
        g_s2e->getDebugStream(g_s2e_state) << "flushTlbCachePage: Erasing cache entry for " << objectState << "\n";
#endif
        m_tlbMap.erase(tlbIt);
    }
}

/**
 * If a page contains at least one byte of symbolic data, it will go through
 * the slow path. Otherwise, softmmu will directly access the concrete array.
 */
void S2EExecutionStateTlb::updateTlbEntryConcreteStatus(struct CPUX86State *env, unsigned mmu_idx, unsigned index,
                                                        const klee::ObjectStateConstPtr &state) {
    CPUTLBEntry *te = &env->tlb_table[mmu_idx][index];

    if (!state->isAllConcrete()) {
        te->addr_read |= TLB_SYMB;
        te->addr_write |= TLB_SYMB;
        te->addr_code |= TLB_SYMB;
    } else {
        te->addr_read &= ~TLB_SYMB;
        te->addr_write &= ~TLB_SYMB;
        te->addr_code &= ~TLB_SYMB;
    }

#ifdef SE_ENABLE_PHYSRAM_TLB
    uintptr_t ram_tlb_index = (state->getAddress() >> 12) & (CPU_TLB_SIZE - 1);
    CPUTLBRAMEntry *re = &env->se_ram_tlb[ram_tlb_index];
    re->host_page = 0;
    re->addend = 0;
    re->object_state = 0;
#endif
}

#if defined(SE_ENABLE_PHYSRAM_TLB)
void S2EExecutionStateTlb::clearRamTlb() {
    static CPUTLBRAMEntry nullptrCPUTLBRAMEntry = {0, 0, nullptr};
    for (unsigned i = 0; i < CPU_TLB_SIZE; i++) {
        env->se_ram_tlb[i] = nullptrCPUTLBRAMEntry;
    }
}
#endif

void S2EExecutionStateTlb::clearTlbOwnership() {
    CPUX86State *env = m_registers->getCpuState();

    for (unsigned i = 0; i < CPU_TLB_SIZE; i++) {
        for (unsigned mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
            env->tlb_table[mmu_idx][i].addr_write |= TLB_NOT_OURS;
        }
    }
}

void S2EExecutionStateTlb::updateTlbEntry(CPUX86State *env, int mmu_idx, uint64_t virtAddr, uint64_t hostAddr) {
    assert((hostAddr & ~TARGET_PAGE_MASK) == 0);
    assert((virtAddr & ~TARGET_PAGE_MASK) == 0);

    unsigned int index = (virtAddr >> SE_RAM_OBJECT_BITS) & (CPU_TLB_SIZE - 1);

    CPUTLBEntry *entry = &env->tlb_table[mmu_idx][index];
    ObjectState *oldObjectState = static_cast<ObjectState *>(entry->objectState);

    /* Retrieve the object state using the host address */
    auto newObjectState = m_asCache->get(hostAddr);
    assert(newObjectState);

/* Store the new mapping in the cache */
#ifdef S2E_DEBUG_TLBCACHE
    g_s2e->getDebugStream(g_s2e_state) << "updateTlbEntry: replacing " << hexval(oldObjectState) << " with "
                                       << hexval(newObjectState) << " (" << mmu_idx << ',' << index << ")\n";
#endif

    if (oldObjectState != newObjectState) {
        flushTlbCachePage(oldObjectState, mmu_idx, index);
        if (!g_s2e_single_path_mode) {
            m_tlbMap[newObjectState].push_back(TlbCoordinates(mmu_idx, index));
        }
    }

#ifdef S2E_DEBUG_TLBCACHE
    const ObjectStateTlbReferences &refs = m_tlbMap[const_cast<ObjectState *>(newObjectState)];
    for (unsigned i = 0; i < refs.size(); ++i) {
        g_s2e->getDebugStream(g_s2e_state) << "   "
                                           << " (" << refs[i].first << ',' << refs[i].second << ")\n";
    }
#endif

    /* Update the TLB entry */
    entry->objectState = const_cast<ObjectState *>(newObjectState.get());

    if (newObjectState->isSharedConcrete()) {
        entry->se_addend = (hostAddr - virtAddr);
    } else {
        entry->se_addend = ((uintptr_t) newObjectState->getConcreteStore(true) - virtAddr);

        if (m_asCache->isOwnedByUs(newObjectState)) {
            entry->addr_write &= ~TLB_NOT_OURS;
        } else {
            entry->addr_write |= TLB_NOT_OURS;
        }
    }

    updateTlbEntryConcreteStatus(env, mmu_idx, index, newObjectState);

#ifdef S2E_DEBUG_TLBCACHE
    audit();
#endif
}

bool S2EExecutionStateTlb::audit() {
    /**
     * Go through the TLB and make sure that all object states are
     * properly referenced.
     */
    CPUX86State *env = m_registers->getCpuState();

    for (auto tlbIt : m_tlbMap) {
        auto os = tlbIt.first;
        const ObjectStateTlbReferences &vec = tlbIt.second;
        foreach2 (vit, vec.begin(), vec.end()) {
            unsigned mmu_idx = (*vit).first;
            unsigned index = (*vit).second;

            CPUTLBEntry *entry = &env->tlb_table[mmu_idx][index];
            assert(entry->objectState == os);
            (void) entry;
            (void) os;
        }
    }

    for (unsigned i = 0; i < CPU_TLB_SIZE; i++) {
        for (unsigned mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
            CPUTLBEntry *entry = &env->tlb_table[mmu_idx][i];
            if (!entry->objectState) {
                continue;
            }

            TlbMap::const_iterator it = m_tlbMap.find((ObjectState *) entry->objectState);
            assert(it != m_tlbMap.end());

            const ObjectStateTlbReferences &vec = (*it).second;
            unsigned foundCount = 0;
            foreach2 (vit, vec.begin(), vec.end()) {
                if ((*vit).first == mmu_idx && (*vit).second == i) {
                    foundCount++;
                }
            }
            assert(foundCount == 1);
        }
    }

    return true;
}
}
