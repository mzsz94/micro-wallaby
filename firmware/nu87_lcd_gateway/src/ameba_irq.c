/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/irq.h>
#include <zephyr/arch/arm/cortex_m/exception.h>

extern int __real_arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
		void (*routine)(const void *), const void *parameter, uint32_t flags);

int __wrap_arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
		void (*routine)(const void *), const void *parameter, uint32_t flags)
{
	/* Pinned Ameba-D hci_uart.c uses UART1 IRQ 35 and INT_PRI_LOWEST (7).
	 * That is a hardware priority, whereas Zephyr adds _IRQ_PRIO_OFFSET.
	 * Preserve the intended lowest priority without changing any other IRQ
	 * registration or weakening Zephyr's range checks.
	 */
	if (irq == 35U && priority == 7U && flags == 0U) {
		priority = IRQ_PRIO_LOWEST;
	}
	return __real_arch_irq_connect_dynamic(irq, priority, routine, parameter, flags);
}
