/******************************************************************************
 * arch/x86/x86_64/mm.c
 * 
 * Modifications to Linux original are copyright (c) 2004, K A Fraser tr This 
 * program is free software; you can redistribute it and/or modify it under 
 * the terms of the GNU General Public License as published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along 
 * with this program; If not, see <http://www.gnu.org/licenses/>.
 */

EMIT_FILE;

#include <xen/lib.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/numa.h>
#include <xen/nodemask.h>
#include <xen/guest_access.h>
#include <xen/mem_access.h>
#include <asm/current.h>
#include <asm/asm_defns.h>
#include <asm/page.h>
#include <asm/flushtlb.h>
#include <asm/fixmap.h>
#include <asm/msr.h>
#include <asm/pv/domain.h>
#include <asm/setup.h>
#include <asm/numa.h>
#include <asm/mem_paging.h>
#include <asm/mem_sharing.h>
#include <asm/trampoline.h>

#include <public/memory.h>

#ifdef CONFIG_PV32

#define compat_machine_to_phys_mapping ((unsigned int *)RDWR_COMPAT_MPT_VIRT_START)

unsigned int __initdata m2p_compat_vstart = __HYPERVISOR_COMPAT_VIRT_START;

l2_pgentry_t *__read_mostly compat_idle_pg_table_l2;

#else /* !CONFIG_PV32 */

/*
 * Declare the symbol such that (dead) code referencing it can be built
 * without a lot of #ifdef-ary (relying on DCE by the compiler).
 */
extern unsigned int compat_machine_to_phys_mapping[];

#endif /* CONFIG_PV32 */

void *do_page_walk(struct vcpu *v, unsigned long addr)
{
    unsigned long mfn = pagetable_get_pfn(v->arch.guest_table);
    l4_pgentry_t l4e, *l4t;
    l3_pgentry_t l3e, *l3t;
    l2_pgentry_t l2e, *l2t;
    l1_pgentry_t l1e, *l1t;

    if ( !is_pv_vcpu(v) || !is_canonical_address(addr) )
        return NULL;

    l4t = map_domain_page(_mfn(mfn));
    l4e = l4t[l4_table_offset(addr)];
    unmap_domain_page(l4t);
    if ( !(l4e_get_flags(l4e) & _PAGE_PRESENT) )
        return NULL;

    l3t = map_l3t_from_l4e(l4e);
    l3e = l3t[l3_table_offset(addr)];
    unmap_domain_page(l3t);
    mfn = l3e_get_pfn(l3e);
    if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) || !mfn_valid(_mfn(mfn)) )
        return NULL;
    if ( (l3e_get_flags(l3e) & _PAGE_PSE) )
    {
        mfn += PFN_DOWN(addr & ((1UL << L3_PAGETABLE_SHIFT) - 1));
        goto ret;
    }

    l2t = map_domain_page(_mfn(mfn));
    l2e = l2t[l2_table_offset(addr)];
    unmap_domain_page(l2t);
    mfn = l2e_get_pfn(l2e);
    if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) || !mfn_valid(_mfn(mfn)) )
        return NULL;
    if ( (l2e_get_flags(l2e) & _PAGE_PSE) )
    {
        mfn += PFN_DOWN(addr & ((1UL << L2_PAGETABLE_SHIFT) - 1));
        goto ret;
    }

    l1t = map_domain_page(_mfn(mfn));
    l1e = l1t[l1_table_offset(addr)];
    unmap_domain_page(l1t);
    mfn = l1e_get_pfn(l1e);
    if ( !(l1e_get_flags(l1e) & _PAGE_PRESENT) || !mfn_valid(_mfn(mfn)) )
        return NULL;

 ret:
    return map_domain_page(_mfn(mfn)) + (addr & ~PAGE_MASK);
}

/*
 * Allocate page table pages for m2p table
 */
struct mem_hotadd_info
{
    unsigned long spfn;
    unsigned long epfn;
    unsigned long cur;
};

static int hotadd_mem_valid(unsigned long pfn, struct mem_hotadd_info *info)
{
    return (pfn < info->epfn && pfn >= info->spfn);
}

static mfn_t alloc_hotadd_mfn(struct mem_hotadd_info *info)
{
    mfn_t mfn;

    ASSERT((info->cur + ( 1UL << PAGETABLE_ORDER) < info->epfn) &&
            info->cur >= info->spfn);

    mfn = _mfn(info->cur);
    info->cur += (1UL << PAGETABLE_ORDER);
    return mfn;
}

#define M2P_NO_MAPPED   0
#define M2P_2M_MAPPED   1
#define M2P_1G_MAPPED   2
static int m2p_mapped(unsigned long spfn)
{
    unsigned long va;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;

    va = RO_MPT_VIRT_START + spfn * sizeof(*machine_to_phys_mapping);
    l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(va)], l3_table_offset(va));

    switch ( l3e_get_flags(l3e) & (_PAGE_PRESENT | _PAGE_PSE) )
    {
        case _PAGE_PSE|_PAGE_PRESENT:
            return M2P_1G_MAPPED;
        /* Check for next level */
        case _PAGE_PRESENT:
            break;
        default:
            return M2P_NO_MAPPED;
    }
    l2e = l2e_from_l3e(l3e, l2_table_offset(va));

    if ( l2e_get_flags(l2e) & _PAGE_PRESENT )
        return M2P_2M_MAPPED;

    return M2P_NO_MAPPED;
}

static int share_hotadd_m2p_table(struct mem_hotadd_info *info)
{
    unsigned long i, n, v;
    mfn_t m2p_start_mfn = INVALID_MFN;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;

    /* M2P table is mappable read-only by privileged domains. */
    for ( v  = RDWR_MPT_VIRT_START;
          v != RDWR_MPT_VIRT_END;
          v += n << PAGE_SHIFT )
    {
        n = L2_PAGETABLE_ENTRIES * L1_PAGETABLE_ENTRIES;
        l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(v)],
                           l3_table_offset(v));
        if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
            continue;
        if ( !(l3e_get_flags(l3e) & _PAGE_PSE) )
        {
            n = L1_PAGETABLE_ENTRIES;
            l2e = l2e_from_l3e(l3e, l2_table_offset(v));
            if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) )
                continue;
            m2p_start_mfn = l2e_get_mfn(l2e);
        }
        else
            continue;

        for ( i = 0; i < n; i++ )
        {
            struct page_info *page = mfn_to_page(mfn_add(m2p_start_mfn, i));

            if ( hotadd_mem_valid(mfn_x(mfn_add(m2p_start_mfn, i)), info) )
                share_xen_page_with_privileged_guests(page, SHARE_ro);
        }
    }

    for ( v  = RDWR_COMPAT_MPT_VIRT_START;
          v != RDWR_COMPAT_MPT_VIRT_END;
          v += 1 << L2_PAGETABLE_SHIFT )
    {
        l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(v)],
                           l3_table_offset(v));
        if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
            continue;
        l2e = l2e_from_l3e(l3e, l2_table_offset(v));
        if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) )
            continue;
        m2p_start_mfn = l2e_get_mfn(l2e);

        for ( i = 0; i < L1_PAGETABLE_ENTRIES; i++ )
        {
            struct page_info *page = mfn_to_page(mfn_add(m2p_start_mfn, i));

            if ( hotadd_mem_valid(mfn_x(mfn_add(m2p_start_mfn, i)), info) )
                share_xen_page_with_privileged_guests(page, SHARE_ro);
        }
    }
    return 0;
}

static void destroy_compat_m2p_mapping(struct mem_hotadd_info *info)
{
    unsigned long i, smap = info->spfn, emap = info->spfn;

    if ( !opt_pv32 ||
         smap > ((RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2) )
        return;

    if ( emap > ((RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2) )
        emap = (RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2;

    for ( i = smap; i < emap; )
    {
        unsigned int off = i * sizeof(*compat_machine_to_phys_mapping);
        l2_pgentry_t *pl2e = compat_idle_pg_table_l2 + l2_table_offset(off);

        if ( l2e_get_flags(*pl2e) & _PAGE_PRESENT )
        {
            unsigned long pt_pfn = l2e_get_pfn(*pl2e);

            if ( hotadd_mem_valid(pt_pfn, info) )
            {
                unsigned long rwva = RDWR_COMPAT_MPT_VIRT_START + off;

                destroy_xen_mappings(rwva, rwva + (1UL << L2_PAGETABLE_SHIFT));
                l2e_write(pl2e, l2e_empty());
            }
        }

        i += 1UL << (L2_PAGETABLE_SHIFT - 2);
    }

    return;
}

static void destroy_m2p_mapping(struct mem_hotadd_info *info)
{
    l3_pgentry_t *l3_ro_mpt;
    unsigned long i, va, rwva;
    unsigned long smap = info->spfn, emap = info->epfn;

    l3_ro_mpt = map_l3t_from_l4e(
                    idle_pg_table[l4_table_offset(RO_MPT_VIRT_START)]);

    /*
     * No need to clean m2p structure existing before the hotplug
     */
    for (i = smap; i < emap;)
    {
        unsigned long pt_pfn;
        l2_pgentry_t *pl2e;

        va = RO_MPT_VIRT_START + i * sizeof(*machine_to_phys_mapping);
        rwva = RDWR_MPT_VIRT_START + i * sizeof(*machine_to_phys_mapping);

        /* 1G mapping should not be created by mem hotadd */
        if (!(l3e_get_flags(l3_ro_mpt[l3_table_offset(va)]) & _PAGE_PRESENT) ||
            (l3e_get_flags(l3_ro_mpt[l3_table_offset(va)]) & _PAGE_PSE))
        {
            i = ( i & ~((1UL << (L3_PAGETABLE_SHIFT - 3)) - 1)) +
                (1UL << (L3_PAGETABLE_SHIFT - 3) );
            continue;
        }

        pl2e = map_l2t_from_l3e(l3_ro_mpt[l3_table_offset(va)]) +
                    l2_table_offset(va);
        if ( !(l2e_get_flags(*pl2e) & _PAGE_PRESENT) )
        {
            i = ( i & ~((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1)) +
                    (1UL << (L2_PAGETABLE_SHIFT - 3)) ;
            UNMAP_DOMAIN_PAGE(pl2e);
            continue;
        }

        pt_pfn = l2e_get_pfn(*pl2e);
        if ( hotadd_mem_valid(pt_pfn, info) )
        {
            destroy_xen_mappings(rwva, rwva + (1UL << L2_PAGETABLE_SHIFT));

            l2e_write(pl2e, l2e_empty());
        }
        i = ( i & ~((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1)) +
              (1UL << (L2_PAGETABLE_SHIFT - 3));
        unmap_domain_page(pl2e);
    }

    UNMAP_DOMAIN_PAGE(l3_ro_mpt);

    destroy_compat_m2p_mapping(info);

    /* Brute-Force flush all TLB */
    flush_tlb_all();
    return;
}

/*
 * Allocate and map the compatibility mode machine-to-phys table.
 * spfn/epfn: the pfn ranges to be setup
 * free_s/free_e: the pfn ranges that is free still
 */
static int setup_compat_m2p_table(struct mem_hotadd_info *info)
{
    unsigned long i, smap, emap, epfn = info->epfn;
    mfn_t mfn;
    unsigned int n;
    int err = 0;

    smap = info->spfn & (~((1UL << (L2_PAGETABLE_SHIFT - 2)) -1));

    /*
     * Notice: For hot-added memory, only range below m2p_compat_vstart
     * will be filled up (assuming memory is discontinous when booting).
     */
    if ( !opt_pv32 ||
         (smap > ((RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2)) )
        return 0;

    if ( epfn > ((RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2) )
        epfn = (RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2;

    emap = ( (epfn + ((1UL << (L2_PAGETABLE_SHIFT - 2)) - 1 )) &
                ~((1UL << (L2_PAGETABLE_SHIFT - 2)) - 1) );

#define MFN(x) (((x) << L2_PAGETABLE_SHIFT) / sizeof(unsigned int))
#define CNT (ISOLATE_LSB(sizeof(*frame_table)) / \
             sizeof(*compat_machine_to_phys_mapping))
    BUILD_BUG_ON(ISOLATE_LSB(sizeof(*frame_table)) % \
                 sizeof(*compat_machine_to_phys_mapping));

    for ( i = smap; i < emap; i += (1UL << (L2_PAGETABLE_SHIFT - 2)) )
    {
        unsigned int off = i * sizeof(*compat_machine_to_phys_mapping);
        l2_pgentry_t *pl2e = compat_idle_pg_table_l2 + l2_table_offset(off);
        unsigned long rwva = RDWR_COMPAT_MPT_VIRT_START + off;

        if ( l2e_get_flags(*pl2e) & _PAGE_PRESENT )
            continue;

        for ( n = 0; n < CNT; ++n)
            if ( mfn_valid(_mfn(i + n * PDX_GROUP_COUNT)) )
                break;
        if ( n == CNT )
            continue;

        mfn = alloc_hotadd_mfn(info);
        err = map_pages_to_xen(rwva, mfn, 1UL << PAGETABLE_ORDER,
                               PAGE_HYPERVISOR);
        if ( err )
            break;
        /* Fill with INVALID_M2P_ENTRY. */
        memset((void *)rwva, 0xFF, 1UL << L2_PAGETABLE_SHIFT);
        /* NB. Cannot be GLOBAL as the ptes get copied into per-VM space. */
        l2e_write(pl2e, l2e_from_mfn(mfn, _PAGE_PSE|_PAGE_PRESENT));
    }
#undef CNT
#undef MFN
    return err;
}

/*
 * Allocate and map the machine-to-phys table.
 * The L3 for RO/RWRW MPT and the L2 for compatible MPT should be setup already
 */
static int setup_m2p_table(struct mem_hotadd_info *info)
{
    unsigned long i, va, smap, emap;
    unsigned int n;
    l2_pgentry_t *l2_ro_mpt = NULL;
    l3_pgentry_t *l3_ro_mpt = NULL;
    int ret = 0;

    ASSERT(l4e_get_flags(idle_pg_table[l4_table_offset(RO_MPT_VIRT_START)])
            & _PAGE_PRESENT);
    l3_ro_mpt = map_l3t_from_l4e(
                    idle_pg_table[l4_table_offset(RO_MPT_VIRT_START)]);

    smap = (info->spfn & (~((1UL << (L2_PAGETABLE_SHIFT - 3)) -1)));
    emap = ((info->epfn + ((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1 )) &
                ~((1UL << (L2_PAGETABLE_SHIFT - 3)) -1));

    va = RO_MPT_VIRT_START + smap * sizeof(*machine_to_phys_mapping);

#define MFN(x) (((x) << L2_PAGETABLE_SHIFT) / sizeof(unsigned long))
#define CNT (ISOLATE_LSB(sizeof(*frame_table)) / \
             sizeof(*machine_to_phys_mapping))

    BUILD_BUG_ON(ISOLATE_LSB(sizeof(*frame_table)) % \
                 sizeof(*machine_to_phys_mapping));

    i = smap;
    while ( i < emap )
    {
        if ( (RO_MPT_VIRT_START + i * sizeof(*machine_to_phys_mapping)) &
             ((1UL << L3_PAGETABLE_SHIFT) - 1) )
            UNMAP_DOMAIN_PAGE(l2_ro_mpt);

        switch ( m2p_mapped(i) )
        {
        case M2P_1G_MAPPED:
            i = ( i & ~((1UL << (L3_PAGETABLE_SHIFT - 3)) - 1)) +
                (1UL << (L3_PAGETABLE_SHIFT - 3));
            continue;
        case M2P_2M_MAPPED:
            i = (i & ~((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1)) +
                (1UL << (L2_PAGETABLE_SHIFT - 3));
            continue;
        default:
            break;
        }

        va = RO_MPT_VIRT_START + i * sizeof(*machine_to_phys_mapping);

        for ( n = 0; n < CNT; ++n)
            if ( mfn_valid(_mfn(i + n * PDX_GROUP_COUNT)) )
                break;
        if ( n < CNT )
        {
            mfn_t mfn = alloc_hotadd_mfn(info);

            ret = map_pages_to_xen(
                        RDWR_MPT_VIRT_START + i * sizeof(unsigned long),
                        mfn, 1UL << PAGETABLE_ORDER,
                        PAGE_HYPERVISOR);
            if ( ret )
                goto error;
            /* Fill with INVALID_M2P_ENTRY. */
            memset((void *)(RDWR_MPT_VIRT_START + i * sizeof(unsigned long)),
                   0xFF, 1UL << L2_PAGETABLE_SHIFT);

            ASSERT(!(l3e_get_flags(l3_ro_mpt[l3_table_offset(va)]) &
                  _PAGE_PSE));
            if ( l2_ro_mpt )
                /* nothing */;
            else if ( l3e_get_flags(l3_ro_mpt[l3_table_offset(va)]) &
                      _PAGE_PRESENT )
                l2_ro_mpt = map_l2t_from_l3e(l3_ro_mpt[l3_table_offset(va)]);
            else
            {
                mfn_t l2_ro_mpt_mfn;

                l2_ro_mpt = alloc_mapped_pagetable(&l2_ro_mpt_mfn);
                if ( !l2_ro_mpt )
                {
                    ret = -ENOMEM;
                    goto error;
                }

                l3e_write(&l3_ro_mpt[l3_table_offset(va)],
                          l3e_from_mfn(l2_ro_mpt_mfn,
                                       __PAGE_HYPERVISOR_RO | _PAGE_USER));
            }

            /* NB. Cannot be GLOBAL: guest user mode should not see it. */
            l2e_write(&l2_ro_mpt[l2_table_offset(va)], l2e_from_mfn(mfn,
                   /*_PAGE_GLOBAL|*/_PAGE_PSE|_PAGE_USER|_PAGE_PRESENT));
        }
        i += ( 1UL << (L2_PAGETABLE_SHIFT - 3));
    }
#undef CNT
#undef MFN

    ret = setup_compat_m2p_table(info);
error:
    unmap_domain_page(l2_ro_mpt);
    unmap_domain_page(l3_ro_mpt);
    return ret;
}

void __init paging_init(void)
{
    unsigned long i, mpt_size, va;
    unsigned int n, memflags = 0;
    l3_pgentry_t *l3_ro_mpt;
    l2_pgentry_t *pl2e = NULL, *l2_ro_mpt = NULL;
    struct page_info *l1_pg;
    mfn_t mfn;

    /*
     * We setup the L3s for 1:1 mapping if host support memory hotplug
     * to avoid sync the 1:1 mapping on page fault handler
     */
    for ( va = DIRECTMAP_VIRT_START;
          va < DIRECTMAP_VIRT_END && (void *)va < __va(mem_hotplug);
          va += (1UL << L4_PAGETABLE_SHIFT) )
    {
        if ( !(l4e_get_flags(idle_pg_table[l4_table_offset(va)]) &
              _PAGE_PRESENT) )
        {
            l3_pgentry_t *pl3t = alloc_mapped_pagetable(&mfn);

            if ( !pl3t )
                goto nomem;
            UNMAP_DOMAIN_PAGE(pl3t);
            l4e_write(&idle_pg_table[l4_table_offset(va)],
                      l4e_from_mfn(mfn, __PAGE_HYPERVISOR_RW));
        }
    }

    /* Create user-accessible L2 directory to map the MPT for guests. */
    l3_ro_mpt = alloc_mapped_pagetable(&mfn);
    if ( !l3_ro_mpt )
        goto nomem;
    l4e_write(&idle_pg_table[l4_table_offset(RO_MPT_VIRT_START)],
              l4e_from_mfn(mfn, __PAGE_HYPERVISOR_RO | _PAGE_USER));

    /*
     * Allocate and map the machine-to-phys table.
     * This also ensures L3 is present for fixmaps.
     */
    mpt_size  = (max_page * BYTES_PER_LONG) + (1UL << L2_PAGETABLE_SHIFT) - 1;
    mpt_size &= ~((1UL << L2_PAGETABLE_SHIFT) - 1UL);
#define MFN(x) (((x) << L2_PAGETABLE_SHIFT) / sizeof(unsigned long))
#define CNT (ISOLATE_LSB(sizeof(*frame_table)) / \
             sizeof(*machine_to_phys_mapping))
    BUILD_BUG_ON((sizeof(*frame_table) & ~sizeof(*frame_table)) % \
                 sizeof(*machine_to_phys_mapping));
    for ( i = 0; i < (mpt_size >> L2_PAGETABLE_SHIFT); i++ )
    {
        BUILD_BUG_ON(RO_MPT_VIRT_START & ((1UL << L3_PAGETABLE_SHIFT) - 1));
        va = RO_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT);

        if ( cpu_has_page1gb &&
             !((unsigned long)pl2e & ~PAGE_MASK) &&
             (mpt_size >> L3_PAGETABLE_SHIFT) > (i >> PAGETABLE_ORDER) )
        {
            unsigned int k, holes;

            for ( holes = k = 0; k < 1 << PAGETABLE_ORDER; ++k)
            {
                for ( n = 0; n < CNT; ++n)
                {
                    mfn = _mfn(MFN(i + k) + n * PDX_GROUP_COUNT);
                    if ( mfn_valid(mfn) )
                        break;
                }
                if ( n == CNT )
                    ++holes;
                else if ( k == holes )
                    memflags = MEMF_node(mfn_to_nid(mfn));
            }
            if ( k == holes )
            {
                i += (1UL << PAGETABLE_ORDER) - 1;
                continue;
            }
            if ( holes == 0 &&
                 (l1_pg = alloc_domheap_pages(NULL, 2 * PAGETABLE_ORDER,
                                              memflags)) != NULL )
            {
                map_pages_to_xen(
                    RDWR_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT),
                    page_to_mfn(l1_pg),
                    1UL << (2 * PAGETABLE_ORDER),
                    PAGE_HYPERVISOR);
                /* Fill with INVALID_M2P_ENTRY. */
                memset((void *)(RDWR_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT)),
                       0xFF, 1UL << L3_PAGETABLE_SHIFT);

                ASSERT(!l2_table_offset(va));
                /* NB. Cannot be GLOBAL: guest user mode should not see it. */
                l3e_write(&l3_ro_mpt[l3_table_offset(va)],
                    l3e_from_page(l1_pg,
                        /*_PAGE_GLOBAL|*/_PAGE_PSE|_PAGE_USER|_PAGE_PRESENT));
                i += (1UL << PAGETABLE_ORDER) - 1;
                continue;
            }
        }

        for ( n = 0; n < CNT; ++n)
        {
            mfn = _mfn(MFN(i) + n * PDX_GROUP_COUNT);
            if ( mfn_valid(mfn) )
            {
                memflags = MEMF_node(mfn_to_nid(mfn));
                break;
            }
        }
        if ( n == CNT )
            l1_pg = NULL;
        else if ( (l1_pg = alloc_domheap_pages(NULL, PAGETABLE_ORDER,
                                               memflags)) == NULL )
            goto nomem;
        else
        {
            map_pages_to_xen(
                RDWR_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT),
                page_to_mfn(l1_pg),
                1UL << PAGETABLE_ORDER,
                PAGE_HYPERVISOR);
            /* Fill with INVALID_M2P_ENTRY. */
            memset((void *)(RDWR_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT)),
                   0xFF, 1UL << L2_PAGETABLE_SHIFT);
        }
        if ( !((unsigned long)pl2e & ~PAGE_MASK) )
        {
            UNMAP_DOMAIN_PAGE(l2_ro_mpt);

            l2_ro_mpt = alloc_mapped_pagetable(&mfn);
            if ( !l2_ro_mpt )
                goto nomem;

            l3e_write(&l3_ro_mpt[l3_table_offset(va)],
                      l3e_from_mfn(mfn, __PAGE_HYPERVISOR_RO | _PAGE_USER));
            pl2e = l2_ro_mpt;
            ASSERT(!l2_table_offset(va));
        }
        /* NB. Cannot be GLOBAL: guest user mode should not see it. */
        if ( l1_pg )
            l2e_write(pl2e, l2e_from_page(
                l1_pg, /*_PAGE_GLOBAL|*/_PAGE_PSE|_PAGE_USER|_PAGE_PRESENT));
        pl2e++;
    }
#undef CNT
#undef MFN
    UNMAP_DOMAIN_PAGE(l2_ro_mpt);
    UNMAP_DOMAIN_PAGE(l3_ro_mpt);

    /* Create user-accessible L2 directory to map the MPT for compat guests. */
    if ( opt_pv32 )
    {
        mfn = alloc_xen_pagetable();
        if ( mfn_eq(mfn, INVALID_MFN) )
            goto nomem;
        compat_idle_pg_table_l2 = map_domain_page_global(mfn);
        clear_page(compat_idle_pg_table_l2);
        pl2e = compat_idle_pg_table_l2;

        /* Allocate and map the compatibility mode machine-to-phys table. */
        mpt_size = (mpt_size >> 1) + (1UL << (L2_PAGETABLE_SHIFT - 1));
        if ( mpt_size > RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START )
            mpt_size = RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START;
        mpt_size &= ~((1UL << L2_PAGETABLE_SHIFT) - 1UL);
        if ( (m2p_compat_vstart + mpt_size) < MACH2PHYS_COMPAT_VIRT_END )
            m2p_compat_vstart = MACH2PHYS_COMPAT_VIRT_END - mpt_size;
    }
    else
        mpt_size = 0;

#define MFN(x) (((x) << L2_PAGETABLE_SHIFT) / sizeof(unsigned int))
#define CNT (ISOLATE_LSB(sizeof(*frame_table)) / \
             sizeof(*compat_machine_to_phys_mapping))
    BUILD_BUG_ON((sizeof(*frame_table) & ~sizeof(*frame_table)) % \
                 sizeof(*compat_machine_to_phys_mapping));
    for ( i = 0; i < (mpt_size >> L2_PAGETABLE_SHIFT); i++, pl2e++ )
    {
        for ( n = 0; n < CNT; ++n)
        {
            mfn = _mfn(MFN(i) + n * PDX_GROUP_COUNT);
            if ( mfn_valid(mfn) )
            {
                memflags = MEMF_node(mfn_to_nid(mfn));
                break;
            }
        }
        if ( n == CNT )
            continue;
        if ( (l1_pg = alloc_domheap_pages(NULL, PAGETABLE_ORDER,
                                          memflags)) == NULL )
            goto nomem;
        map_pages_to_xen(
            RDWR_COMPAT_MPT_VIRT_START + (i << L2_PAGETABLE_SHIFT),
            page_to_mfn(l1_pg),
            1UL << PAGETABLE_ORDER,
            PAGE_HYPERVISOR);
        /* Fill with INVALID_M2P_ENTRY. */
        memset((void *)(RDWR_COMPAT_MPT_VIRT_START +
                        (i << L2_PAGETABLE_SHIFT)),
               0xFF, 1UL << L2_PAGETABLE_SHIFT);
        /* NB. Cannot be GLOBAL as the ptes get copied into per-VM space. */
        l2e_write(pl2e, l2e_from_page(l1_pg, _PAGE_PSE|_PAGE_PRESENT));
    }
#undef CNT
#undef MFN

    machine_to_phys_mapping_valid = 1;

    /* Set up linear page table mapping. */
    l4e_write(&idle_pg_table[l4_table_offset(LINEAR_PT_VIRT_START)],
              l4e_from_paddr(__pa(idle_pg_table), __PAGE_HYPERVISOR_RW));
    return;

 nomem:
    panic("Not enough memory for m2p table\n");
}

void __init zap_low_mappings(void)
{
    BUG_ON(num_online_cpus() != 1);

    /* Stop using l?_bootmap[] mappings. */
    l4e_write(&idle_pg_table[0], l4e_empty());
    flush_local(FLUSH_TLB_GLOBAL);

    /*
     * Insert an identity mapping of the AP/S3 part of the trampoline, which
     * is arranged to fit in a single page.
     */
    map_pages_to_xen(trampoline_phys, maddr_to_mfn(trampoline_phys),
                     1, __PAGE_HYPERVISOR_RX);
}

int setup_compat_arg_xlat(struct vcpu *v)
{
    return create_perdomain_mapping(v->domain, ARG_XLAT_START(v),
                                    PFN_UP(COMPAT_ARG_XLAT_SIZE),
                                    NULL, NIL(struct page_info *));
}

void free_compat_arg_xlat(struct vcpu *v)
{
    destroy_perdomain_mapping(v->domain, ARG_XLAT_START(v),
                              PFN_UP(COMPAT_ARG_XLAT_SIZE));
}

static void cleanup_frame_table(struct mem_hotadd_info *info)
{
    unsigned long sva, eva;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;
    mfn_t spfn, epfn;

    spfn = _mfn(info->spfn);
    epfn = _mfn(info->epfn);

    sva = (unsigned long)mfn_to_page(spfn);
    eva = (unsigned long)mfn_to_page(epfn);

    /* Intialize all page */
    memset((void *)sva, -1, eva - sva);

    while (sva < eva)
    {
        l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(sva)],
                           l3_table_offset(sva));
        if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) ||
             (l3e_get_flags(l3e) & _PAGE_PSE) )
        {
            sva = (sva & ~((1UL << L3_PAGETABLE_SHIFT) - 1)) +
                    (1UL << L3_PAGETABLE_SHIFT);
            continue;
        }

        l2e = l2e_from_l3e(l3e, l2_table_offset(sva));
        ASSERT(l2e_get_flags(l2e) & _PAGE_PRESENT);

        if ( (l2e_get_flags(l2e) & (_PAGE_PRESENT | _PAGE_PSE)) ==
              (_PAGE_PSE | _PAGE_PRESENT) )
        {
            if (hotadd_mem_valid(l2e_get_pfn(l2e), info))
                destroy_xen_mappings(sva & ~((1UL << L2_PAGETABLE_SHIFT) - 1),
                         ((sva & ~((1UL << L2_PAGETABLE_SHIFT) -1 )) +
                            (1UL << L2_PAGETABLE_SHIFT) - 1));

            sva = (sva & ~((1UL << L2_PAGETABLE_SHIFT) -1 )) +
                  (1UL << L2_PAGETABLE_SHIFT);
            continue;
        }

        ASSERT(l1e_get_flags(l1e_from_l2e(l2e, l1_table_offset(sva))) &
               _PAGE_PRESENT);

        sva = (sva & PAGE_MASK) + PAGE_SIZE;
    }

    /* Brute-Force flush all TLB */
    flush_tlb_all();
}

static int setup_frametable_chunk(void *start, void *end,
                                  struct mem_hotadd_info *info)
{
    unsigned long s = (unsigned long)start;
    unsigned long e = (unsigned long)end;
    mfn_t mfn;
    int err;

    ASSERT(!(s & ((1 << L2_PAGETABLE_SHIFT) - 1)));
    ASSERT(!(e & ((1 << L2_PAGETABLE_SHIFT) - 1)));

    for ( ; s < e; s += (1UL << L2_PAGETABLE_SHIFT))
    {
        mfn = alloc_hotadd_mfn(info);
        err = map_pages_to_xen(s, mfn, 1UL << PAGETABLE_ORDER,
                               PAGE_HYPERVISOR);
        if ( err )
            return err;
    }
    memset(start, -1, s - (unsigned long)start);

    return 0;
}

static int extend_frame_table(struct mem_hotadd_info *info)
{
    unsigned long cidx, nidx, eidx;
    mfn_t spfn, epfn;

    spfn = _mfn(info->spfn);
    epfn = _mfn(info->epfn);

    eidx = DIV_ROUND_UP(mfn_to_pdx(epfn), PDX_GROUP_COUNT);
    nidx = cidx = mfn_to_pdx(spfn)/PDX_GROUP_COUNT;

    ASSERT( mfn_to_pdx(epfn) <= (DIRECTMAP_SIZE >> PAGE_SHIFT) &&
            mfn_to_pdx(epfn) <= FRAMETABLE_NR );

    if ( test_bit(cidx, pdx_group_valid) )
        cidx = find_next_zero_bit(pdx_group_valid, eidx, cidx);

    if ( cidx >= eidx )
        return 0;

    while ( cidx < eidx )
    {
        int err;

        nidx = find_next_bit(pdx_group_valid, eidx, cidx);
        if ( nidx >= eidx )
            nidx = eidx;
        err = setup_frametable_chunk(pdx_to_page(cidx * PDX_GROUP_COUNT ),
                                     pdx_to_page(nidx * PDX_GROUP_COUNT),
                                     info);
        if ( err )
            return err;

        cidx = find_next_zero_bit(pdx_group_valid, eidx, nidx);
    }

    memset(mfn_to_page(spfn), 0,
           (unsigned long)mfn_to_page(epfn) - (unsigned long)mfn_to_page(spfn));
    return 0;
}

void __init subarch_init_memory(void)
{
    unsigned long i, n, v, m2p_start_mfn;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;

    BUILD_BUG_ON(RDWR_MPT_VIRT_START & ((1UL << L3_PAGETABLE_SHIFT) - 1));
    BUILD_BUG_ON(RDWR_MPT_VIRT_END   & ((1UL << L3_PAGETABLE_SHIFT) - 1));
    /* M2P table is mappable read-only by privileged domains. */
    for ( v  = RDWR_MPT_VIRT_START;
          v != RDWR_MPT_VIRT_END;
          v += n << PAGE_SHIFT )
    {
        n = L2_PAGETABLE_ENTRIES * L1_PAGETABLE_ENTRIES;
        l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(v)],
                           l3_table_offset(v));
        if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
            continue;
        if ( !(l3e_get_flags(l3e) & _PAGE_PSE) )
        {
            n = L1_PAGETABLE_ENTRIES;
            l2e = l2e_from_l3e(l3e, l2_table_offset(v));
            if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) )
                continue;
            m2p_start_mfn = l2e_get_pfn(l2e);
        }
        else
        {
            m2p_start_mfn = l3e_get_pfn(l3e);
        }

        for ( i = 0; i < n; i++ )
            share_xen_page_with_privileged_guests(
                mfn_to_page(_mfn(m2p_start_mfn + i)), SHARE_ro);
    }

    if ( opt_pv32 )
        for ( v  = RDWR_COMPAT_MPT_VIRT_START;
              v != RDWR_COMPAT_MPT_VIRT_END;
              v += 1 << L2_PAGETABLE_SHIFT )
        {
            l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(v)],
                               l3_table_offset(v));
            if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
                continue;
            l2e = l2e_from_l3e(l3e, l2_table_offset(v));
            if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT) )
                continue;
            m2p_start_mfn = l2e_get_pfn(l2e);

            for ( i = 0; i < L1_PAGETABLE_ENTRIES; i++ )
                share_xen_page_with_privileged_guests(
                    mfn_to_page(_mfn(m2p_start_mfn + i)), SHARE_ro);
        }

    /* Mark all of direct map NX if hardware supports it. */
    if ( !cpu_has_nx )
        return;

    for ( i = l4_table_offset(DIRECTMAP_VIRT_START);
          i < l4_table_offset(DIRECTMAP_VIRT_END); ++i )
    {
        l4_pgentry_t l4e = idle_pg_table[i];

        if ( l4e_get_flags(l4e) & _PAGE_PRESENT )
        {
            l4e_add_flags(l4e, _PAGE_NX_BIT);
            idle_pg_table[i] = l4e;
        }
    }
}

long subarch_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    struct xen_machphys_mfn_list xmml;
    l3_pgentry_t l3e;
    l2_pgentry_t l2e;
    unsigned long v, limit;
    xen_pfn_t mfn, last_mfn;
    unsigned int i;
    long rc = 0;

    switch ( cmd )
    {
    case XENMEM_machphys_mfn_list:
        if ( copy_from_guest(&xmml, arg, 1) )
            return -EFAULT;

        BUILD_BUG_ON(RDWR_MPT_VIRT_START & ((1UL << L3_PAGETABLE_SHIFT) - 1));
        BUILD_BUG_ON(RDWR_MPT_VIRT_END   & ((1UL << L3_PAGETABLE_SHIFT) - 1));
        for ( i = 0, v = RDWR_MPT_VIRT_START, last_mfn = 0;
              (i != xmml.max_extents) &&
              (v < (unsigned long)(machine_to_phys_mapping + max_page));
              i++, v += 1UL << L2_PAGETABLE_SHIFT )
        {
            l3e = l3e_from_l4e(idle_pg_table[l4_table_offset(v)],
                               l3_table_offset(v));
            if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
                mfn = last_mfn;
            else if ( !(l3e_get_flags(l3e) & _PAGE_PSE) )
            {
                l2e = l2e_from_l3e(l3e, l2_table_offset(v));
                if ( l2e_get_flags(l2e) & _PAGE_PRESENT )
                    mfn = l2e_get_pfn(l2e);
                else
                    mfn = last_mfn;
            }
            else
            {
                mfn = l3e_get_pfn(l3e)
                    + (l2_table_offset(v) << PAGETABLE_ORDER);
            }
            ASSERT(mfn);
            if ( copy_to_guest_offset(xmml.extent_start, i, &mfn, 1) )
                return -EFAULT;
            last_mfn = mfn;
        }

        xmml.nr_extents = i;
        if ( __copy_to_guest(arg, &xmml, 1) )
            return -EFAULT;

        break;

    case XENMEM_machphys_compat_mfn_list:
        if ( !opt_pv32 )
            return -EOPNOTSUPP;

        if ( copy_from_guest(&xmml, arg, 1) )
            return -EFAULT;

        limit = (unsigned long)(compat_machine_to_phys_mapping + max_page);
        if ( limit > RDWR_COMPAT_MPT_VIRT_END )
            limit = RDWR_COMPAT_MPT_VIRT_END;
        for ( i = 0, v = RDWR_COMPAT_MPT_VIRT_START, last_mfn = 0;
              (i != xmml.max_extents) && (v < limit);
              i++, v += 1 << L2_PAGETABLE_SHIFT )
        {
            l2e = compat_idle_pg_table_l2[l2_table_offset(v)];
            if ( l2e_get_flags(l2e) & _PAGE_PRESENT )
                mfn = l2e_get_pfn(l2e);
            else
                mfn = last_mfn;
            ASSERT(mfn);
            if ( copy_to_guest_offset(xmml.extent_start, i, &mfn, 1) )
                return -EFAULT;
            last_mfn = mfn;
        }

        xmml.nr_extents = i;
        if ( __copy_to_guest(arg, &xmml, 1) )
            rc = -EFAULT;

        break;

    case XENMEM_get_sharing_freed_pages:
        return mem_sharing_get_nr_saved_mfns();

    case XENMEM_get_sharing_shared_pages:
        return mem_sharing_get_nr_shared_mfns();

#ifdef CONFIG_MEM_PAGING
    case XENMEM_paging_op:
        return mem_paging_memop(guest_handle_cast(arg, xen_mem_paging_op_t));
#endif

#ifdef CONFIG_MEM_SHARING
    case XENMEM_sharing_op:
        return mem_sharing_memop(guest_handle_cast(arg, xen_mem_sharing_op_t));
#endif

    default:
        rc = -ENOSYS;
        break;
    }

    return rc;
}

int pagefault_by_memadd(unsigned long addr, struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;

    return mem_hotplug && guest_mode(regs) && is_pv_32bit_domain(d) &&
           (addr >= HYPERVISOR_COMPAT_VIRT_START(d)) &&
           (addr < MACH2PHYS_COMPAT_VIRT_END);
}

int handle_memadd_fault(unsigned long addr, struct cpu_user_regs *regs)
{
    struct domain *d = current->domain;
    l4_pgentry_t *pl4e = NULL;
    l4_pgentry_t l4e;
    l3_pgentry_t  *pl3e = NULL;
    l3_pgentry_t l3e;
    l2_pgentry_t *pl2e = NULL;
    l2_pgentry_t l2e, idle_l2e;
    unsigned long mfn, idle_index;
    int ret = 0;

    if (!is_pv_32bit_domain(d))
        return 0;

    if ( (addr < HYPERVISOR_COMPAT_VIRT_START(d)) ||
         (addr >= MACH2PHYS_COMPAT_VIRT_END) )
        return 0;

    mfn = (read_cr3()) >> PAGE_SHIFT;

    pl4e = map_domain_page(_mfn(mfn));

    l4e = pl4e[0];

    if (!(l4e_get_flags(l4e) & _PAGE_PRESENT))
        goto unmap;

    mfn = l4e_get_pfn(l4e);
    /* We don't need get page type here since it is current CR3 */
    pl3e = map_domain_page(_mfn(mfn));

    l3e = pl3e[3];

    if ( !(l3e_get_flags(l3e) & _PAGE_PRESENT) )
        goto unmap;

    mfn = l3e_get_pfn(l3e);
    pl2e = map_domain_page(_mfn(mfn));

    l2e = pl2e[l2_table_offset(addr)];

    if ( !(l2e_get_flags(l2e) & _PAGE_PRESENT))
        goto unmap;

    idle_index = (l2_table_offset(addr) -
                        COMPAT_L2_PAGETABLE_FIRST_XEN_SLOT(d))/
                  sizeof(l2_pgentry_t);
    idle_l2e = compat_idle_pg_table_l2[idle_index];
    if (!(l2e_get_flags(idle_l2e) & _PAGE_PRESENT))
        goto unmap;

    memcpy(&pl2e[l2_table_offset(addr)],
            &compat_idle_pg_table_l2[idle_index],
            sizeof(l2_pgentry_t));

    ret = EXCRET_fault_fixed;

unmap:
    if ( pl4e )
        unmap_domain_page(pl4e);
    if ( pl3e )
        unmap_domain_page(pl3e);
    if ( pl2e )
        unmap_domain_page(pl2e);

    return ret;
}

void domain_set_alloc_bitsize(struct domain *d)
{
    if ( !is_pv_32bit_domain(d) ||
         (MACH2PHYS_COMPAT_NR_ENTRIES(d) >= max_page) ||
         d->arch.physaddr_bitsize > 0 )
        return;
    d->arch.physaddr_bitsize =
        /* 2^n entries can be contained in guest's p2m mapping space */
        fls(MACH2PHYS_COMPAT_NR_ENTRIES(d)) - 1
        /* 2^n pages -> 2^(n+PAGE_SHIFT) bits */
        + PAGE_SHIFT;
}

unsigned int domain_clamp_alloc_bitsize(struct domain *d, unsigned int bits)
{
    if ( (d == NULL) || (d->arch.physaddr_bitsize == 0) )
        return bits;
    return min(d->arch.physaddr_bitsize, bits);
}

static int transfer_pages_to_heap(struct mem_hotadd_info *info)
{
    unsigned long i;
    struct page_info *pg;

    /*
     * Mark the allocated page before put free pages to buddy allocator
     * to avoid merge in free_heap_pages
     */
    for (i = info->spfn; i < info->cur; i++)
    {
        pg = mfn_to_page(_mfn(i));
        pg->count_info = PGC_state_inuse;
    }

    init_domheap_pages(pfn_to_paddr(info->cur), pfn_to_paddr(info->epfn));

    return 0;
}

static int mem_hotadd_check(unsigned long spfn, unsigned long epfn)
{
    unsigned long s, e, length, sidx, eidx;
    paddr_t mem_base = pfn_to_paddr(spfn);
    unsigned long mem_npages = epfn - spfn;

    if ( (spfn >= epfn) )
        return 0;

    if (pfn_to_pdx(epfn) > FRAMETABLE_NR)
        return 0;

    if ( (spfn | epfn) & ((1UL << PAGETABLE_ORDER) - 1) )
        return 0;

    if ( !pdx_is_region_compressible(mem_base, mem_npages) )
        return 0;

    /* Make sure the new range is not present now */
    sidx = ((pfn_to_pdx(spfn) + PDX_GROUP_COUNT - 1)  & ~(PDX_GROUP_COUNT - 1))
            / PDX_GROUP_COUNT;
    eidx = (pfn_to_pdx(epfn - 1) & ~(PDX_GROUP_COUNT - 1)) / PDX_GROUP_COUNT;
    if (sidx >= eidx)
        return 0;

    s = find_next_zero_bit(pdx_group_valid, eidx, sidx);
    if ( s > eidx )
        return 0;
    e = find_next_bit(pdx_group_valid, eidx, s);
    if ( e < eidx )
        return 0;

    /* Caculate at most required m2p/compat m2p/frametable pages */
    s = (spfn & ~((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1));
    e = (epfn + (1UL << (L2_PAGETABLE_SHIFT - 3)) - 1) &
            ~((1UL << (L2_PAGETABLE_SHIFT - 3)) - 1);

    length = (e - s) * sizeof(unsigned long);

    s = (spfn & ~((1UL << (L2_PAGETABLE_SHIFT - 2)) - 1));
    e = (epfn + (1UL << (L2_PAGETABLE_SHIFT - 2)) - 1) &
            ~((1UL << (L2_PAGETABLE_SHIFT - 2)) - 1);

    e = min_t(unsigned long, e,
            (RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) >> 2);

    if ( e > s )
        length += (e -s) * sizeof(unsigned int);

    s = pfn_to_pdx(spfn) & ~(PDX_GROUP_COUNT - 1);
    e = ( pfn_to_pdx(epfn) + (PDX_GROUP_COUNT - 1) ) & ~(PDX_GROUP_COUNT - 1);

    length += (e - s) * sizeof(struct page_info);

    if ( (length >> PAGE_SHIFT) > mem_npages )
        return 0;

    return 1;
}

/*
 * A bit paranoid for memory allocation failure issue since
 * it may be reason for memory add
 */
int memory_add(unsigned long spfn, unsigned long epfn, unsigned int pxm)
{
    struct mem_hotadd_info info;
    int ret;
    nodeid_t node;
    unsigned long old_max = max_page, old_total = total_pages;
    unsigned long old_node_start, old_node_span, orig_online;
    unsigned long i;

    dprintk(XENLOG_INFO, "memory_add %lx ~ %lx with pxm %x\n", spfn, epfn, pxm);

    if ( !mem_hotadd_check(spfn, epfn) )
        return -EINVAL;

    if ( (node = setup_node(pxm)) == NUMA_NO_NODE )
        return -EINVAL;

    if ( !valid_numa_range(spfn << PAGE_SHIFT, epfn << PAGE_SHIFT, node) )
    {
        printk(XENLOG_WARNING
               "pfn range %lx..%lx PXM %x node %x is not NUMA-valid\n",
               spfn, epfn, pxm, node);
        return -EINVAL;
    }

    i = virt_to_mfn(HYPERVISOR_VIRT_END - 1) + 1;
    if ( spfn < i )
    {
        ret = map_pages_to_xen((unsigned long)mfn_to_virt(spfn), _mfn(spfn),
                               min(epfn, i) - spfn, PAGE_HYPERVISOR);
        if ( ret )
            goto destroy_directmap;
    }
    if ( i < epfn )
    {
        if ( i < spfn )
            i = spfn;
        ret = map_pages_to_xen((unsigned long)mfn_to_virt(i), _mfn(i),
                               epfn - i, __PAGE_HYPERVISOR_RW);
        if ( ret )
            goto destroy_directmap;
    }

    old_node_start = node_start_pfn(node);
    old_node_span = node_spanned_pages(node);
    orig_online = node_online(node);

    if ( !orig_online )
    {
        dprintk(XENLOG_WARNING, "node %x pxm %x is not online\n",node, pxm);
        NODE_DATA(node)->node_start_pfn = spfn;
        NODE_DATA(node)->node_spanned_pages =
                epfn - node_start_pfn(node);
        node_set_online(node);
    }
    else
    {
        if (node_start_pfn(node) > spfn)
            NODE_DATA(node)->node_start_pfn = spfn;
        if (node_end_pfn(node) < epfn)
            NODE_DATA(node)->node_spanned_pages = epfn - node_start_pfn(node);
    }

    info.spfn = spfn;
    info.epfn = epfn;
    info.cur = spfn;

    ret = extend_frame_table(&info);
    if (ret)
        goto destroy_frametable;

    /* Set max_page as setup_m2p_table will use it*/
    if (max_page < epfn)
    {
        max_page = epfn;
        max_pdx = pfn_to_pdx(max_page - 1) + 1;
    }
    total_pages += epfn - spfn;

    set_pdx_range(spfn, epfn);
    ret = setup_m2p_table(&info);

    if ( ret )
        goto destroy_m2p;

    /*
     * If hardware domain has IOMMU mappings but page tables are not
     * shared or being kept in sync then newly added memory needs to be
     * mapped here.
     */
    if ( is_iommu_enabled(hardware_domain) &&
         !iommu_use_hap_pt(hardware_domain) &&
         !need_iommu_pt_sync(hardware_domain) )
    {
        for ( i = spfn; i < epfn; i++ )
            if ( iommu_legacy_map(hardware_domain, _dfn(i), _mfn(i),
                                  1UL << PAGE_ORDER_4K,
                                  IOMMUF_readable | IOMMUF_writable) )
                break;
        if ( i != epfn )
        {
            while (i-- > old_max)
                /* If statement to satisfy __must_check. */
                if ( iommu_legacy_unmap(hardware_domain, _dfn(i),
                                        1UL << PAGE_ORDER_4K) )
                    continue;

            goto destroy_m2p;
        }
    }

    /* We can't revert any more */
    share_hotadd_m2p_table(&info);
    transfer_pages_to_heap(&info);

    /* Update the node's present pages (like the total_pages of the system) */
    NODE_DATA(node)->node_present_pages += epfn - spfn;

    return 0;

destroy_m2p:
    destroy_m2p_mapping(&info);
    max_page = old_max;
    total_pages = old_total;
    max_pdx = pfn_to_pdx(max_page - 1) + 1;
destroy_frametable:
    cleanup_frame_table(&info);
    if ( !orig_online )
        node_set_offline(node);
    NODE_DATA(node)->node_start_pfn = old_node_start;
    NODE_DATA(node)->node_spanned_pages = old_node_span;
 destroy_directmap:
    destroy_xen_mappings((unsigned long)mfn_to_virt(spfn),
                         (unsigned long)mfn_to_virt(epfn));

    return ret;
}

void set_gpfn_from_mfn(unsigned long mfn, unsigned long pfn)
{
    unsigned long entry = pfn;

    if ( unlikely(!machine_to_phys_mapping_valid) )
        return;

    if ( entry != INVALID_M2P_ENTRY )
    {
        const struct domain *d = page_get_owner(mfn_to_page(_mfn(mfn)));

        if ( d && (d == dom_cow) )
            entry = SHARED_M2P_ENTRY;
    }

    if ( opt_pv32 &&
         mfn < (RDWR_COMPAT_MPT_VIRT_END - RDWR_COMPAT_MPT_VIRT_START) / 4 )
        compat_machine_to_phys_mapping[mfn] = entry;

    machine_to_phys_mapping[mfn] = entry;
}

#ifdef CONFIG_COMPAT
#include "compat/mm.c"
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
