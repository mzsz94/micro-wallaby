"""Compile the production HCI IRQ adapter and exercise its forwarding contract."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src" / "ameba_irq.c"

HARNESS = r"""
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/irq.h>
#include <zephyr/arch/arm/cortex_m/exception.h>

int __wrap_arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
    void (*routine)(const void *), const void *parameter, uint32_t flags);

static unsigned int seen_irq, seen_priority, calls;
static uint32_t seen_flags;
static void (*seen_routine)(const void *);
static const void *seen_parameter;
static int real_result;
static unsigned int handler_calls;
static const void *handler_parameter;

int __real_arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
    void (*routine)(const void *), const void *parameter, uint32_t flags)
{
    calls++;
    seen_irq = irq;
    seen_priority = priority;
    seen_flags = flags;
    seen_routine = routine;
    seen_parameter = parameter;
    return real_result;
}

static void handler(const void *parameter)
{
    handler_calls++;
    handler_parameter = parameter;
}

static void check(unsigned int irq, unsigned int priority, uint32_t flags,
    unsigned int expected_priority, int result, int use_callback)
{
    static const uint32_t parameter = UINT32_C(0x87210035);
    void (*callback)(const void *) = use_callback ? handler : NULL;
    const void *argument = use_callback ? &parameter : NULL;
    unsigned int before_calls = calls;
    unsigned int before_handler_calls = handler_calls;

    real_result = result;
    assert(__wrap_arch_irq_connect_dynamic(irq, priority, callback, argument, flags) == result);
    assert(calls == before_calls + 1);
    assert(seen_irq == irq);
    assert(seen_priority == expected_priority);
    assert(seen_flags == flags);
    assert(seen_routine == callback);
    assert(seen_parameter == argument);
    /* Registration must pass the ISR through, without executing it. */
    assert(handler_calls == before_handler_calls);
    if (seen_routine != NULL) {
        seen_routine(seen_parameter);
        assert(handler_calls == before_handler_calls + 1);
        assert(handler_parameter == argument);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "vendor_hci") == 0) {
        /* Hardware regression: physical priority 7 becomes Zephyr priority 6.
         * The Zephyr kernel will add its reserved priority offset afterward. */
        check(35, 7, 0, IRQ_PRIO_LOWEST, 35, 1);
        check(35, 7, 0, IRQ_PRIO_LOWEST, -22, 1);
        check(35, 7, 0, IRQ_PRIO_LOWEST, 0, 0);
    } else {
        assert(strcmp(argv[1], "passthrough") == 0);
        const unsigned int irqs[] = {0, 34, 36, 63, UINT_MAX};
        const unsigned int priorities[] = {0, 1, 5, 6, 8, UINT_MAX};
        const uint32_t flags[] = {1, 2, UINT32_C(0x80000000), UINT32_MAX};
        for (unsigned int i = 0; i < sizeof(irqs) / sizeof(irqs[0]); i++) {
            check(irqs[i], 7, 0, 7, -16, 1);
        }
        for (unsigned int i = 0; i < sizeof(priorities) / sizeof(priorities[0]); i++) {
            check(35, priorities[i], 0, priorities[i], 123, 1);
        }
        for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
            check(35, 7, flags[i], 7, -5, 1);
        }
        check(0, 0, 0, 0, 0, 0);
    }
    puts(argv[1]);
    return 0;
}
"""


class AmebaIrqTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if compiler is None:
            raise unittest.SkipTest("A host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="ameba-irq-test-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        headers = directory / "zephyr"
        headers.mkdir()
        (headers / "irq.h").write_text(
            "#pragma once\n#include <stdint.h>\n"
            "int arch_irq_connect_dynamic(unsigned int, unsigned int, "
            "void (*)(const void *), const void *, uint32_t);\n",
            encoding="utf-8",
        )
        architecture = headers / "arch" / "arm" / "cortex_m"
        architecture.mkdir(parents=True)
        (architecture / "exception.h").write_text(
            "#pragma once\n#define IRQ_PRIO_LOWEST 6U\n", encoding="utf-8"
        )
        harness = directory / "harness.c"
        harness.write_text(HARNESS, encoding="utf-8")
        cls.executable = directory / "test_ameba_irq"
        completed = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2",
             "-I", str(directory), str(SOURCE), str(harness), "-o", str(cls.executable)],
            capture_output=True, text=True, timeout=60, check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(f"Production IRQ adapter did not compile:\n{completed.stdout}{completed.stderr}")

    def run_case(self, name):
        completed = subprocess.run(
            [str(self.executable), name], capture_output=True, text=True,
            timeout=10, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(completed.stdout.strip(), name)

    def test_hci_priority_translation_preserves_call_and_result(self):
        self.run_case("vendor_hci")

    def test_other_interrupts_priorities_and_flags_are_unchanged(self):
        self.run_case("passthrough")


if __name__ == "__main__":
    unittest.main()
