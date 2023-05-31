#include <xen/compile.h>
#include <xen/init.h>

#include <asm/early_printk.h>
#include <asm/mm.h>

/* Xen stack for bringing up the first CPU. */
unsigned char __initdata cpu0_boot_stack[STACK_SIZE]
    __aligned(STACK_SIZE);

/*  
 * To be sure that .bss isn't zero. It will simplify code of
 * .bss initialization.
 * TODO:
 *   To be deleted when the first real .bss user appears
 */
int dummy_bss __attribute__((unused));

void __init noreturn start_xen(unsigned long bootcpu_id,
                               paddr_t dtb_addr)
{
    early_printk("Hello from C env\n");

    setup_initial_pagetables();

    enable_mmu();

    for ( ;; )
        asm volatile ("wfi");

    unreachable();
}

void __init noreturn cont_after_mmu_is_enabled(void)
{
    early_printk("All set up\n");

    for ( ;; )
        asm volatile ("wfi");

    unreachable();
}
