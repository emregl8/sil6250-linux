// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irq_work.h>
#include <linux/module.h>

#include "sil6250_irq.h"

struct test_irq {
	struct completion fired;
	atomic_t calls;
	atomic_t startups;
	atomic_t masked;
	atomic_t pending;
	struct irq_work work;
	int irq;
};

static void test_mask(struct irq_data *data)
{
	struct test_irq *test = irq_data_get_irq_chip_data(data);

	atomic_set(&test->masked, 1);
}

static void test_unmask(struct irq_data *data)
{
	struct test_irq *test = irq_data_get_irq_chip_data(data);

	atomic_set(&test->masked, 0);
	if (atomic_read(&test->pending))
		irq_work_queue(&test->work);
}

static void test_inject(struct irq_work *work)
{
	struct test_irq *test = container_of(work, struct test_irq, work);

	if (!atomic_read(&test->masked) && atomic_xchg(&test->pending, 0))
		generic_handle_irq_safe(test->irq);
}

static unsigned int test_startup(struct irq_data *data)
{
	struct test_irq *test = irq_data_get_irq_chip_data(data);

	atomic_inc(&test->startups);
	test_unmask(data);
	return 0;
}

static void test_raise(struct test_irq *test)
{
	atomic_set(&test->pending, 1);
	if (!atomic_read(&test->masked))
		irq_work_queue(&test->work);
}

static struct irq_chip test_chip = {
	.name = "sil6250-irq-test",
	.irq_mask = test_mask,
	.irq_unmask = test_unmask,
	.irq_startup = test_startup,
	.irq_shutdown = test_mask,
};

static irqreturn_t test_handler(int irq, void *data)
{
	struct test_irq *test = data;

	disable_irq_nosync(irq);
	atomic_inc(&test->calls);
	complete(&test->fired);
	return IRQ_HANDLED;
}

static int exercise_irq(struct device *dev, bool legacy)
{
	struct test_irq test;
	bool requested = false;
	int irq, ret;

	init_completion(&test.fired);
	atomic_set(&test.calls, 0);
	atomic_set(&test.startups, 0);
	atomic_set(&test.masked, 1);
	atomic_set(&test.pending, 0);
	init_irq_work(&test.work, test_inject);
	irq = irq_alloc_desc(NUMA_NO_NODE);
	if (irq < 0)
		return irq;
	test.irq = irq;
	irq_set_chip_data(irq, &test);
	irq_set_chip_and_handler(irq, &test_chip, handle_level_irq);
	irq_clear_status_flags(irq, IRQ_NOREQUEST | IRQ_NOPROBE);
	irq_set_status_flags(irq, IRQ_LEVEL);
	if (legacy)
		ret = devm_request_threaded_irq(dev, irq, NULL, test_handler,
						IRQF_ONESHOT, "sil6250-irq-test", &test);
	else
		ret = sil6250_request_masked_irq(dev, irq, test_handler,
						"sil6250-irq-test", &test);
	if (ret)
		goto out;
	requested = true;
	if (!legacy && atomic_read(&test.startups) != 0) {
		ret = -EPROTO;
		goto out;
	}

	test_raise(&test);
	if (legacy) {
		if (!wait_for_completion_timeout(&test.fired, msecs_to_jiffies(1000))) {
			ret = -ETIMEDOUT;
			goto out;
		}
		disable_irq(irq);
	} else if (wait_for_completion_timeout(&test.fired, msecs_to_jiffies(5)) ||
		   atomic_read(&test.calls) != 0) {
		ret = -EPROTO;
		goto out;
	}

	reinit_completion(&test.fired);
	enable_irq(irq);
	if (legacy) {
		test_raise(&test);
		if (wait_for_completion_timeout(&test.fired, msecs_to_jiffies(20)) ||
		    atomic_read(&test.calls) != 1)
			ret = -EPROTO;
	} else {
		if (!wait_for_completion_timeout(&test.fired, msecs_to_jiffies(1000)) ||
		    atomic_read(&test.calls) != 1 || atomic_read(&test.startups) != 1) {
			ret = -ETIMEDOUT;
			goto out;
		}
		reinit_completion(&test.fired);
		test_raise(&test);
		enable_irq(irq);
		if (!wait_for_completion_timeout(&test.fired, msecs_to_jiffies(1000)) ||
		    atomic_read(&test.calls) != 2 || atomic_read(&test.startups) != 1)
			ret = -ETIMEDOUT;
	}

out:
	if (requested)
		devm_free_irq(dev, irq, &test);
	irq_work_sync(&test.work);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
	irq_free_desc(irq);
	return ret;
}

static int __init irq_startup_test_init(void)
{
	struct device *dev;
	int ret, attempt;

	dev = root_device_register("sil6250-irq-test");
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	ret = exercise_irq(dev, true);
	if (ret)
		goto out;
	pr_info("sil6250_irq_test: original startup race reproduced\n");
	for (attempt = 0; attempt < 64; attempt++) {
		ret = exercise_irq(dev, false);
		if (ret)
			goto out;
	}
	pr_info("sil6250_irq_test: masked startup and rearm passed 64 iterations\n");
out:
	root_device_unregister(dev);
	if (ret)
		pr_err("sil6250_irq_test: failed: %d\n", ret);
	return ret;
}

static void __exit irq_startup_test_exit(void)
{
}

module_init(irq_startup_test_init);
module_exit(irq_startup_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SIL6250 synthetic IRQ startup regression test");
