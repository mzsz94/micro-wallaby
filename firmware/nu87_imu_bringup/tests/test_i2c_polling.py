#!/usr/bin/env python3
"""Run the actual AmebaD polling functions against a host register model.

This compiles verbatim functions from the patched Zephyr driver as C++ only so
IC_DATA_CMD writes can be observed via a register proxy. Other registers and HAL
calls model successful, aborted, or stalled hardware; no serial device is opened.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "build/nu87-workspace/zephyr/drivers/i2c/i2c_ameba.c"


def function(source, name):
    start = source.index("static int " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREAMBLE = r"""
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <vector>
#define BIT(n) (1UL << (n))
#define I2C_MSG_WRITE 0
#define I2C_MSG_READ BIT(0)
#define I2C_MSG_RW_MASK BIT(0)
#define I2C_MSG_STOP BIT(1)
#define I2C_MSG_RESTART BIT(2)
#define BIT_CTRL_IC_DATA_CMD_CMD BIT(8)
#define BIT_CTRL_IC_DATA_CMD_STOP BIT(9)
#define BIT_CTRL_IC_DATA_CMD_RESTART BIT(10)
#define I2C_BIT_TFNF BIT(1)
#define I2C_BIT_RFNE BIT(3)
#define I2C_BIT_TX_ABRT BIT(6)
#define I2C_BIT_STOP_DET BIT(9)
#define DISABLE 0
#define ENABLE 1
#define K_FOREVER -1
struct i2c_msg { uint8_t *buf; uint32_t len; uint8_t flags; };
static std::vector<uint32_t> commands;
static std::vector<int> enable_calls;
static int clears, lock_balance;
static int64_t ticks;
struct DataRegister {
  DataRegister &operator=(uint32_t value) {
    commands.push_back(value);
    return *this;
  }
  operator uint32_t() const { return 0x6a; }
};
struct I2C_TypeDef {
  uint32_t IC_RAW_INTR_STAT, IC_STATUS, IC_TAR;
  DataRegister IC_DATA_CMD;
};
struct i2c_ameba_config { I2C_TypeDef *I2Cx; };
struct i2c_ameba_data { int master_mode, transfer_lock; };
struct device { i2c_ameba_config *config; i2c_ameba_data *data; };
static void k_mutex_lock(int *, int) { ++lock_balance; }
static void k_mutex_unlock(int *) { --lock_balance; }
static void k_busy_wait(int) {}
static int64_t k_uptime_get() { return ticks++; }
static void I2C_Cmd(I2C_TypeDef *, int enable) { enable_calls.push_back(enable); }
static void I2C_ClearAllINT(I2C_TypeDef *) { ++clears; }
static void I2C_SetSlaveAddress(I2C_TypeDef *reg, uint16_t addr) { reg->IC_TAR=addr; }
"""

HARNESS = r"""
static I2C_TypeDef regs;
static i2c_ameba_config config = {&regs};
static i2c_ameba_data data = {1, 0};
static device dev = {&config, &data};
static void reset() {
  commands.clear(); enable_calls.clear(); clears=0; lock_balance=0; ticks=0;
  regs.IC_RAW_INTR_STAT=I2C_BIT_STOP_DET;
  regs.IC_STATUS=I2C_BIT_TFNF | I2C_BIT_RFNE;
}
static int transfer(i2c_msg *msgs, uint8_t count) {
  int rc=i2c_ameba_validate_msgs(msgs, count);
  if (rc) return rc;
  return i2c_ameba_poll_transfer(&dev, msgs, count, 0x6a);
}
static void expect(const std::vector<uint32_t> &expected) {
  assert(commands==expected);
  assert(regs.IC_TAR==0x6a);
  assert((enable_calls==std::vector<int>{DISABLE,ENABLE,DISABLE}));
  assert(clears==2);
  assert(lock_balance==0);
}
int main() {
  uint8_t reg=0x0f, who=0, sample[12]={}, config_bytes[]={0x10,0x40};
  reset();
  i2c_msg who_msgs[]={{&reg,1,I2C_MSG_WRITE},
                    {&who,1,I2C_MSG_READ|I2C_MSG_RESTART|I2C_MSG_STOP}};
  assert(transfer(who_msgs,2)==0);
  expect({0x40f,0x300});
  assert(who==0x6a);

  reset(); reg=0x22;
  i2c_msg sample_msgs[]={{&reg,1,I2C_MSG_WRITE},
                        {sample,12,I2C_MSG_READ|I2C_MSG_RESTART}};
  assert(transfer(sample_msgs,2)==0);
  expect({0x422,0x100,0x100,0x100,0x100,0x100,0x100,
          0x100,0x100,0x100,0x100,0x100,0x300});

  reset();
  i2c_msg write[]={{config_bytes,2,I2C_MSG_WRITE|I2C_MSG_STOP}};
  assert(transfer(write,1)==0);
  expect({0x10,0x240});

  reset();
  i2c_msg fragments[]={{config_bytes,1,I2C_MSG_WRITE},
                       {config_bytes+1,1,I2C_MSG_WRITE}};
  assert(transfer(fragments,2)==0);
  expect({0x10,0x240});

  reset();
  i2c_msg restart_write[]={{config_bytes,1,I2C_MSG_WRITE},
                           {config_bytes+1,1,I2C_MSG_WRITE|I2C_MSG_RESTART}};
  assert(transfer(restart_write,2)==0);
  expect({0x410,0x240});

  reset();
  i2c_msg read_write[]={{sample,2,I2C_MSG_READ},
                        {config_bytes,2,I2C_MSG_WRITE|I2C_MSG_RESTART}};
  assert(transfer(read_write,2)==0);
  expect({0x100,0x500,0x10,0x240});

  reset();
  i2c_msg missing_restart[]={{&reg,1,I2C_MSG_WRITE},
                             {&who,1,I2C_MSG_READ}};
  assert(transfer(missing_restart,2)==-EINVAL);
  assert(commands.empty());

  reset();
  i2c_msg early_stop[]={{&reg,1,I2C_MSG_WRITE|I2C_MSG_STOP},
                        {&who,1,I2C_MSG_READ|I2C_MSG_RESTART}};
  assert(transfer(early_stop,2)==-EINVAL);
  assert(commands.empty());

  reset();
  regs.IC_RAW_INTR_STAT=I2C_BIT_TX_ABRT;
  assert(transfer(write,1)==-EIO);
  expect({});

  reset();
  regs.IC_RAW_INTR_STAT=0; regs.IC_STATUS=0;
  assert(transfer(write,1)==-ETIMEDOUT);
  expect({});
  assert(ticks<=52);

  reset();
  regs.IC_RAW_INTR_STAT=0;
  assert(transfer(write,1)==-ETIMEDOUT);
  expect({0x10,0x240});
  assert(ticks<=52);
  puts("11 AmebaD polling regression cases passed");
}
"""


def main():
    source = DRIVER.read_text()
    functions = "\n\n".join(function(source, name) for name in (
        "i2c_ameba_validate_msgs", "i2c_ameba_poll_flag", "i2c_ameba_poll_transfer"
    ))
    with tempfile.TemporaryDirectory(prefix="nu87-i2c-polling-") as temporary:
        path = Path(temporary)
        cpp = path / "polling.cpp"
        executable = path / "polling"
        cpp.write_text(PREAMBLE + functions + HARNESS)
        subprocess.run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
