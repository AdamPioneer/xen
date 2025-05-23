/* SPDX-License-Identifier: GPL-2.0-only */
/******************************************************************************
 * irq.h
 * 
 * Interrupt distribution and delivery logic.
 * 
 * Copyright (c) 2006, K A Fraser, XenSource Inc.
 */

#ifndef __ASM_X86_HVM_IRQ_H__
#define __ASM_X86_HVM_IRQ_H__

#include <xen/timer.h>

#include <asm/hvm/hvm.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vioapic.h>

struct hvm_irq {
    /*
     * Virtual interrupt wires for a single PCI bus.
     * Indexed by: device*4 + INTx#.
     */
    struct hvm_hw_pci_irqs pci_intx;

    /*
     * Virtual interrupt wires for ISA devices.
     * Indexed by ISA IRQ (assumes no ISA-device IRQ sharing).
     */
    struct hvm_hw_isa_irqs isa_irq;

    /*
     * PCI-ISA interrupt router.
     * Each PCI <device:INTx#> is 'wire-ORed' into one of four links using
     * the traditional 'barber's pole' mapping ((device + INTx#) & 3).
     * The router provides a programmable mapping from each link to a GSI.
     */
    struct hvm_hw_pci_link pci_link;

    /* Virtual interrupt and via-link for paravirtual platform driver. */
    uint32_t callback_via_asserted;
    union {
        enum {
            HVMIRQ_callback_none,
            HVMIRQ_callback_gsi,
            HVMIRQ_callback_pci_intx,
            HVMIRQ_callback_vector
        } callback_via_type;
    };
    union {
        uint32_t gsi;
        struct { uint8_t dev, intx; } pci;
        uint32_t vector;
    } callback_via;

    /* Number of INTx wires asserting each PCI-ISA link. */
    u8 pci_link_assert_count[4];

    /*
     * GSIs map onto PIC/IO-APIC in the usual way:
     *  0-7:  Master 8259 PIC, IO-APIC pins 0-7
     *  8-15: Slave  8259 PIC, IO-APIC pins 8-15
     *  16+ : IO-APIC pins 16+
     */

    /* Last VCPU that was delivered a LowestPrio interrupt. */
    u8 round_robin_prev_vcpu;

    struct hvm_irq_dpci *dpci;

    /*
     * Number of wires asserting each GSI.
     *
     * GSIs 0-15 are the ISA IRQs. ISA devices map directly into this space
     * except ISA IRQ 0, which is connected to GSI 2.
     * PCI links map into this space via the PCI-ISA bridge.
     *
     * GSIs 16+ are used only be PCI devices. The mapping from PCI device to
     * GSI is as follows: ((device*4 + device/8 + INTx#) & 31) + 16
     */
    unsigned int nr_gsis;
    u8 gsi_assert_count[];
};

#define hvm_pci_intx_gsi(dev, intx)  \
    (((((dev)<<2) + ((dev)>>3) + (intx)) & 31) + 16)
#define hvm_pci_intx_link(dev, intx) \
    (((dev) + (intx)) & 3)
#define hvm_domain_irq(d) ((d)->arch.hvm.irq)

#define hvm_isa_irq_to_gsi(isa_irq) ((isa_irq) ? : 2)

/* Check/Acknowledge next pending interrupt. */
struct hvm_intack hvm_vcpu_has_pending_irq(struct vcpu *v);
struct hvm_intack hvm_vcpu_ack_pending_irq(struct vcpu *v,
                                           struct hvm_intack intack);

struct dev_intx_gsi_link {
    struct list_head list;
    uint8_t bus;
    uint8_t device;
    uint8_t intx;
};

#define _HVM_IRQ_DPCI_MACH_PCI_SHIFT            0
#define _HVM_IRQ_DPCI_MACH_MSI_SHIFT            1
#define _HVM_IRQ_DPCI_MAPPED_SHIFT              2
#define _HVM_IRQ_DPCI_GUEST_PCI_SHIFT           4
#define _HVM_IRQ_DPCI_GUEST_MSI_SHIFT           5
#define _HVM_IRQ_DPCI_IDENTITY_GSI_SHIFT        6
#define _HVM_IRQ_DPCI_NO_EOI_SHIFT              7
#define _HVM_IRQ_DPCI_TRANSLATE_SHIFT          15
#define HVM_IRQ_DPCI_MACH_PCI        (1u << _HVM_IRQ_DPCI_MACH_PCI_SHIFT)
#define HVM_IRQ_DPCI_MACH_MSI        (1u << _HVM_IRQ_DPCI_MACH_MSI_SHIFT)
#define HVM_IRQ_DPCI_MAPPED          (1u << _HVM_IRQ_DPCI_MAPPED_SHIFT)
#define HVM_IRQ_DPCI_GUEST_PCI       (1u << _HVM_IRQ_DPCI_GUEST_PCI_SHIFT)
#define HVM_IRQ_DPCI_GUEST_MSI       (1u << _HVM_IRQ_DPCI_GUEST_MSI_SHIFT)
#define HVM_IRQ_DPCI_IDENTITY_GSI    (1u << _HVM_IRQ_DPCI_IDENTITY_GSI_SHIFT)
#define HVM_IRQ_DPCI_NO_EOI          (1u << _HVM_IRQ_DPCI_NO_EOI_SHIFT)
#define HVM_IRQ_DPCI_TRANSLATE       (1u << _HVM_IRQ_DPCI_TRANSLATE_SHIFT)

struct hvm_gmsi_info {
    uint32_t gvec;
    uint32_t gflags;
    int dest_vcpu_id; /* -1 :multi-dest, non-negative: dest_vcpu_id */
    bool posted; /* directly deliver to guest via VT-d PI? */
};

struct hvm_girq_dpci_mapping {
    struct list_head list;
    uint8_t bus;
    uint8_t device;
    uint8_t intx;
    uint8_t machine_gsi;
};

#define NR_ISA_IRQS 16
#define NR_LINK     4
#define NR_HVM_DOMU_IRQS ARRAY_SIZE(((struct hvm_hw_vioapic *)NULL)->redirtbl)

/* Protected by domain's event_lock */
struct hvm_irq_dpci {
    /* Guest IRQ to guest device/intx mapping. */
    struct list_head girq[NR_HVM_DOMU_IRQS];
    /* Record of mapped ISA IRQs */
    DECLARE_BITMAP(isairq_map, NR_ISA_IRQS);
    /* Record of mapped Links */
    uint8_t link_cnt[NR_LINK];
};

/* Machine IRQ to guest device/intx mapping. */
struct hvm_pirq_dpci {
    uint32_t flags;
    unsigned int state;
    bool masked;
    uint16_t pending;
    struct list_head digl_list;
    struct domain *dom;
    struct hvm_gmsi_info gmsi;
    struct list_head softirq_list;
};

void pt_pirq_init(struct domain *d, struct hvm_pirq_dpci *dpci);
bool pt_pirq_cleanup_check(struct hvm_pirq_dpci *dpci);
int pt_pirq_iterate(struct domain *d,
                    int (*cb)(struct domain *d,
                              struct hvm_pirq_dpci *dpci, void *arg),
                    void *arg);

#ifdef CONFIG_HVM
bool pt_pirq_softirq_active(struct hvm_pirq_dpci *pirq_dpci);
#else
static inline bool pt_pirq_softirq_active(struct hvm_pirq_dpci *pirq_dpci)
{
    return false;
}
#endif

/* Modify state of a PCI INTx wire. */
void hvm_pci_intx_assert(struct domain *d, unsigned int device,
                         unsigned int intx);
void hvm_pci_intx_deassert(struct domain *d, unsigned int device,
                           unsigned int intx);

/*
 * Modify state of an ISA device's IRQ wire. For some cases, we are
 * interested in the interrupt vector of the irq, but once the irq_lock
 * is released, the vector may be changed by others. get_vector() callback
 * allows us to get the interrupt vector in the protection of irq_lock.
 * For most cases, just set get_vector to NULL.
 */
int hvm_isa_irq_assert(struct domain *d, unsigned int isa_irq,
                       int (*get_vector)(const struct domain *d,
                                         unsigned int gsi));
void hvm_isa_irq_deassert(struct domain *d, unsigned int isa_irq);

/* Modify state of GSIs. */
void hvm_gsi_assert(struct domain *d, unsigned int gsi);
void hvm_gsi_deassert(struct domain *d, unsigned int gsi);

int hvm_set_pci_link_route(struct domain *d, u8 link, u8 isa_irq);

int hvm_inject_msi(struct domain *d, uint64_t addr, uint32_t data);

/* Assert/deassert an IO APIC pin. */
int hvm_ioapic_assert(struct domain *d, unsigned int gsi, bool level);
void hvm_ioapic_deassert(struct domain *d, unsigned int gsi);

void hvm_maybe_deassert_evtchn_irq(void);
void hvm_assert_evtchn_irq(struct vcpu *v);
void hvm_set_callback_via(struct domain *d, uint64_t via);

struct pirq;
bool hvm_domain_use_pirq(const struct domain *d, const struct pirq *pirq);

#endif /* __ASM_X86_HVM_IRQ_H__ */
