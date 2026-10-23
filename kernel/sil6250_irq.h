/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SIL6250_IRQ_H
#define SIL6250_IRQ_H

#include <linux/interrupt.h>

static inline int sil6250_request_masked_irq(struct device *dev, unsigned int irq,
					    irq_handler_t thread_fn,
					    const char *name, void *data)
{
	return devm_request_threaded_irq(dev, irq, NULL, thread_fn,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, name, data);
}

#endif
