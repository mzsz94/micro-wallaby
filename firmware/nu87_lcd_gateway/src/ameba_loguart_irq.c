/* SPDX-License-Identifier: Apache-2.0 */
/* The pinned AmebaD HAL declares LOGUART IRQ helpers but exports only the
 * generic UART ROM implementations. Keep this app-local compatibility layer
 * limited to the UART2 console; no pinmux, clock or baud register is changed. */
#include <soc.h>
#include <ameba_soc.h>
#include <zephyr/irq.h>

u32 LOGUART_Writable(void)
{
	return UART_Writable(LOGUART_DEV);
}

u32 LOGUART_GetStatus(UART_TypeDef *uart)
{
	return UART_LineStatusGet(uart);
}

void LOGUART_INTConfig(UART_TypeDef *uart, u32 mask, u32 enabled)
{
	UART_INTConfig(uart, mask, enabled);
}

void LOGUART_INTClear(UART_TypeDef *uart, u32 mask)
{
	/* AmebaD has no Dplus ICR register. RX timeout clears when the FIFO is
	 * drained; the generic driver's Dplus-only LSR[9] condition is never set
	 * by AmebaD's LSR. Reading line status also acknowledges line errors. */
	(void)mask;
	(void)UART_LineStatusGet(uart);
}

u32 irq_disable_save(void)
{
	return irq_lock();
}

void irq_enable_restore(u32 key)
{
	irq_unlock(key);
}
