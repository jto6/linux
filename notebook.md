# Linux Kernel Notebook

Personal notes and learnings about the Linux kernel.

---

## Low Power Mode Support (TI K3 Devices)

**Date**: 2025-10-27

Understanding how TI K3 devices (like J784S4) enter low power modes such as suspend-to-RAM and SOC off states.

### Overview: Dual Firmware Architecture

TI K3 devices use a **two-firmware architecture** for power management:

```
┌─────────────────────────────────────┐
│         Linux (PSCI client)         │
├─────────────────────────────────────┤
│  TI SCI Driver    │   PSCI Driver   │  ← Both in Linux kernel
├───────────────────┴─────────────────┤
│                 │                   │
│     TI DMSC     │    ARM ATF/TF-A   │  ← Separate firmware layers
│  (Device Mgr)   │      (EL3)        │
│  Runs on R5F    │   Runs on A72     │
└─────────────────┴───────────────────┘
```

**Key insight**: Linux communicates with **both** firmware interfaces:
1. **TI SCI** (via mailbox) → Configure SoC-specific resources and power policy
2. **PSCI** (via SMC) → Trigger standard ARM CPU power operations

### Device Tree Configuration

Location: `arch/arm64/boot/dts/ti/k3-j784s4-j742s2-common.dtsi:55-58`

```dts
psci: psci {
    compatible = "arm,psci-1.0";
    method = "smc";          // Use Secure Monitor Call (not HVC)
}
```

All CPU nodes declare: `enable-method = "psci"` (k3-j784s4.dtsi:62, 76, 90, etc.)

### Suspend-to-RAM Code Flow

#### Step 1: Userspace Initiation
```bash
echo mem > /sys/power/state
```

#### Step 2: Kernel PM Core (`kernel/power/suspend.c`)

**Main flow**: `enter_state()` → `suspend_devices_and_enter()` → `suspend_enter()` (line 413)

**`suspend_enter()` responsibilities**:
- Suspend all devices in phases:
  - `dpm_suspend_late(PMSG_SUSPEND)` (line 421)
  - `dpm_suspend_noirq(PMSG_SUSPEND)` (line 430)
- Disable secondary CPUs via `pm_sleep_disable_secondary_cpus()` (line 447)
- Disable interrupts with `arch_suspend_disable_irqs()` (line 451)
- Execute syscore_suspend() (line 456)
- **Critical call**: `suspend_ops->enter(state)` (line 462)
  - This invokes the platform-specific suspend handler
  - For PSCI systems, this is `psci_system_suspend_enter()`

#### Step 3: TI SCI Driver Configuration (`drivers/firmware/ti_sci.c`)

**Before PSCI is invoked**, TI SCI prepares the SoC-specific firmware:

##### 3a. `ti_sci_suspend()` (line 3709)
```c
// Query CPU latency constraints from all CPUs
for_each_possible_cpu(i) {
    val = dev_pm_qos_read_value(cpu_dev, DEV_PM_QOS_RESUME_LATENCY);
    cpu_lat = max(cpu_lat, val);
}

// Send latency constraint to Device Manager
ti_sci_cmd_set_latency_constraint(&info->handle, cpu_lat_ms,
                                  TISCI_MSG_CONSTRAINT_SET);

// Prepare system for suspend
ti_sci_prepare_system_suspend(info);
```

##### 3b. `ti_sci_prepare_system_suspend()` (line 3678)
```c
switch (pm_suspend_target_state) {
case PM_SUSPEND_MEM:
    if (info->fw_caps & MSG_FLAG_CAPS_LPM_DM_MANAGED) {
        // Tell Device Manager: "We're suspending, you choose the power state"
        return ti_sci_cmd_prepare_sleep(&info->handle,
                                       TISCI_MSG_VALUE_SLEEP_MODE_DM_MANAGED,
                                       0, 0, 0);
    }
    break;
}
```

**What DM_MANAGED mode means**: Device Manager firmware (running on R5F core) will select the appropriate SoC power state based on:
- Previously sent latency constraints
- Wake source configuration
- Power domain dependencies
- Current system state

##### 3c. `ti_sci_cmd_prepare_sleep()` (line 1661)
```c
// Allocate message buffer
req = (struct ti_sci_msg_req_prepare_sleep *)xfer->xfer_buf;
req->mode = mode;          // TISCI_MSG_VALUE_SLEEP_MODE_DM_MANAGED
req->ctx_lo = ctx_lo;      // Context address (0 for DM_MANAGED)
req->ctx_hi = ctx_hi;
req->debug_flags = debug_flags;

// Send via mailbox to Device Manager firmware
ret = ti_sci_do_xfer(info, xfer);
```

##### 3d. `ti_sci_suspend_noirq()` (line 3749)
```c
// Configure IO isolation to prevent signal integrity issues
// when power domains are powered down
ti_sci_cmd_set_io_isolation(&info->handle, TISCI_MSG_VALUE_IO_ENABLE);
```

**Why IO isolation?** Prevents floating I/O signals when peripherals lose power, which could cause:
- Leakage current
- Signal integrity issues on shared buses
- Unintended wake events

#### Step 4: PSCI Suspend Entry (`drivers/firmware/psci/psci.c`)

##### 4a. `psci_system_suspend_enter()` (line 540)
```c
static int psci_system_suspend_enter(suspend_state_t state)
{
    // Delegate to ARM64 cpu_suspend framework
    return cpu_suspend(0, psci_system_suspend);
}
```

##### 4b. `psci_system_suspend()` (line 530)
```c
static int psci_system_suspend(unsigned long unused)
{
    int err;
    // Get physical address of resume entry point
    phys_addr_t pa_cpu_resume = __pa_symbol(cpu_resume);

    // Invoke PSCI SYSTEM_SUSPEND via SMC to ATF
    err = invoke_psci_fn(PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND),
                         pa_cpu_resume, 0, 0);
    return psci_to_linux_errno(err);
}
```

**`invoke_psci_fn()`** calls either:
- `__invoke_psci_fn_smc()` (line 124) for SMC
- `__invoke_psci_fn_hvc()` (line 113) for HVC

For J784S4 (method="smc" in DTS), uses `arm_smccc_smc()`:

```c
static __always_inline unsigned long
__invoke_psci_fn_smc(unsigned long function_id,
                     unsigned long arg0, unsigned long arg1,
                     unsigned long arg2)
{
    struct arm_smccc_res res;

    // SMC call to EL3 (ATF)
    arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
    return res.a0;
}
```

#### Step 5: ARM64 CPU Context Save (`arch/arm64/kernel/suspend.c`)

##### 5a. `cpu_suspend()` (line 97)
```c
int cpu_suspend(unsigned long arg, int (*fn)(unsigned long))
{
    int ret = 0;
    unsigned long flags;
    struct sleep_stack_data state;

    // Disable debug exceptions and save state
    flags = local_daif_save();

    // Pause function graph tracer (will lose stack context)
    pause_graph_tracing();

    // Save IRQ context for pseudo-NMI support
    arm_cpuidle_save_irq_context(&context);

    ct_cpuidle_enter();

    if (__cpu_suspend_enter(&state)) {
        // Call the suspend finisher (psci_system_suspend)
        ret = fn(arg);  // This never returns on success!

        // Only reached on error
        if (!ret)
            ret = -EOPNOTSUPP;
        ct_cpuidle_exit();
    } else {
        // Resume path - returned from __cpu_suspend_exit
        ct_cpuidle_exit();
        __cpu_suspend_exit();
    }

    // Restore context
    arm_cpuidle_restore_irq_context(&context);
    unpause_graph_tracing();
    local_daif_restore(flags);

    return ret;
}
```

##### 5b. `__cpu_suspend_enter()` (`arch/arm64/kernel/sleep.S:65`)

Assembly code that saves CPU architectural state:

```assembly
SYM_FUNC_START(__cpu_suspend_enter)
    // Save callee-saved registers
    stp    x29, lr, [x0, #SLEEP_STACK_DATA_CALLEE_REGS]
    stp    x19, x20, [x0, #SLEEP_STACK_DATA_CALLEE_REGS+16]
    stp    x21, x22, [x0, #SLEEP_STACK_DATA_CALLEE_REGS+32]
    stp    x23, x24, [x0, #SLEEP_STACK_DATA_CALLEE_REGS+48]
    stp    x25, x26, [x0, #SLEEP_STACK_DATA_CALLEE_REGS+64]
    stp    x27, x28, [x0, #SLEEP_STACK_DATA_CALLEE_REGS+80]

    // Save stack pointer
    mov    x2, sp
    str    x2, [x0, #SLEEP_STACK_DATA_SYSTEM_REGS + CPU_CTX_SP]

    // Compute hash of MPIDR for per-CPU storage
    ldr_l  x1, sleep_save_stash
    mrs    x7, mpidr_el1
    // ... compute hash ...

    // Store context pointer indexed by CPU
    str    x0, [x1]

    // Call cpu_do_suspend to save MMU state
    add    x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
    bl     cpu_do_suspend

    // Return non-zero to indicate suspend path
    mov    x0, #1
    ret
SYM_FUNC_END(__cpu_suspend_enter)
```

**What `cpu_do_suspend()` saves**:
- Translation table base registers (TTBR0_EL1, TTBR1_EL1)
- Translation control registers (TCR_EL1)
- Memory attribute registers (MAIR_EL1)
- Vector base address (VBAR_EL1)
- Context ID register (CONTEXTIDR_EL1)
- Other system control registers

#### Step 6: Secure Monitor Call to ATF

The `arm_smccc_smc()` executes:
```
SMC #0
  x0 = PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND) = 0xC400000E (ARM64)
  x1 = Physical address of cpu_resume
  x2-x7 = 0
```

This transitions from EL1 (Linux) to EL3 (ARM Trusted Firmware).

**Inside ATF** (not in Linux source tree):
1. PSCI handler validates the request
2. Saves EL3 context
3. Sends final "ready to suspend" message to TI Device Manager (via TI SCI)
4. Device Manager executes the power state transition planned in Step 3
5. Programs SoC power controller
6. Places DDR in self-refresh
7. Powers down CPU clusters
8. Enters SoC low power mode

### Resume Path

#### Step 1: Wake Event → ATF → `cpu_resume`

**Hardware wake sequence**:
1. Wake event (RTC, GPIO, etc.) triggers SoC wake controller
2. Device Manager begins power-up sequence
3. Restores power domains
4. Takes DDR out of self-refresh
5. Releases CPU reset
6. CPU begins execution at `cpu_resume` (physical address passed to PSCI)

#### Step 2: `cpu_resume()` (`arch/arm64/kernel/sleep.S:101`)

Entry point from ATF, runs in **identity-mapped memory** (physical == virtual):

```assembly
SYM_CODE_START(cpu_resume)
    mov    x0, xzr
    bl     init_kernel_el         // Re-initialize EL1
    mov    x19, x0                // Save boot mode
    bl     __cpu_setup             // Re-setup CPU state

    // Enable MMU early so we can access saved context by VA
    adrp   x1, swapper_pg_dir
    adrp   x2, idmap_pg_dir
    bl     __enable_mmu

    // Jump to virtual address
    ldr    x8, =_cpu_resume
    br     x8
SYM_CODE_END(cpu_resume)
```

#### Step 3: `_cpu_resume()` (line 116)

Now running with MMU enabled:

```assembly
SYM_FUNC_START(_cpu_resume)
    mov    x0, x19
    bl     finalise_el2            // Complete EL2 setup

    // Get MPIDR and compute hash to find our saved context
    mrs    x1, mpidr_el1
    // ... compute hash ...

    // Retrieve saved context pointer
    ldr_l  x0, sleep_save_stash
    ldr    x0, [x0, x7, lsl #3]

    // Restore stack pointer
    add    x0, x0, #SLEEP_STACK_DATA_SYSTEM_REGS
    ldr    x2, [x0, #CPU_CTX_SP]
    mov    sp, x2

    // Restore MMU state (TTBR, TCR, etc.)
    bl     cpu_do_resume

    // Restore callee-saved registers
    add    x29, x0, #SLEEP_STACK_DATA_CALLEE_REGS
    ldp    x19, x20, [x29, #16]
    ldp    x21, x22, [x29, #32]
    // ... restore x23-x28 ...
    ldp    x29, lr, [x29]

    // Return 0 to indicate resume path
    mov    x0, #0
    ret
SYM_FUNC_END(_cpu_resume)
```

**Result**: Returns to `cpu_suspend()` in suspend.c, but this time returns 0 (not 1).

#### Step 4: `__cpu_suspend_exit()` (`arch/arm64/kernel/suspend.c:44`)

```c
void notrace __cpu_suspend_exit(void)
{
    unsigned int cpu = smp_processor_id();

    mte_suspend_exit();

    // Uninstall identity mapping (no longer needed)
    cpu_uninstall_idmap();

    // Restore CnP bit in TTBR1_EL1
    if (system_supports_cnp())
        cpu_enable_swapper_cnp();

    // Re-enable detected CPU features
    if (alternative_has_cap_unlikely(ARM64_HAS_DIT))
        set_pstate_dit(1);
    __uaccess_enable_hw_pan();

    // Restore hardware breakpoints
    if (hw_breakpoint_restore)
        hw_breakpoint_restore(cpu);

    // Restore Spectre mitigations
    spectre_v4_enable_mitigation(NULL);

    sme_suspend_exit();
    ptrauth_suspend_exit();
}
```

#### Step 5: TI SCI Resume (`drivers/firmware/ti_sci.c`)

##### `ti_sci_resume_noirq()` (line 3761)
```c
// Disable IO isolation
ti_sci_cmd_set_io_isolation(&info->handle, TISCI_MSG_VALUE_IO_DISABLE);

// Query wake reason from Device Manager
u32 source;
u64 time;
u8 pin, mode;
ret = ti_sci_msg_cmd_lpm_wake_reason(&info->handle, &source, &time, &pin, &mode);
if (!ret)
    dev_info(dev, "wakeup source:0x%x, pin:0x%x, mode:0x%x\n",
             source, pin, mode);
```

#### Step 6: Kernel PM Resume (`kernel/power/suspend.c`)

Back in `suspend_enter()`:
- `syscore_resume()` (line 468)
- Re-enable interrupts via `arch_suspend_enable_irqs()` (line 473)
- Bring secondary CPUs online: `pm_sleep_enable_secondary_cpus()` (line 477)
- Resume devices in reverse order:
  - `dpm_resume_noirq(PMSG_RESUME)` (line 481)
  - `dpm_resume_early(PMSG_RESUME)` (line 487)
- Full device resume back in suspend_devices_and_enter()
- Thaw processes
- Restore console

### Division of Labor: TI SCI vs PSCI

**Critical Understanding**: TI SCI commands are sent **BEFORE** the PSCI call.

#### Why Not Everything Through PSCI?

**PSCI's role**: Standard ARM interface for CPU/cluster power control
- CPU on/off
- CPU idle states
- System suspend (coordinate final CPU power-down)
- Context save/restore entry point

**PSCI limitations**: Generic ARM standard, doesn't know about:
- TI's Device Manager architecture
- TI-specific power domains
- SoC-specific IO isolation
- TI's wake event routing
- Vendor-specific latency requirements

#### TI SCI's Role: SoC-Specific Policy Configuration

**What TI SCI configures BEFORE suspend**:
1. **Latency constraints** - Tell DM the maximum acceptable resume latency
2. **Power state selection** - DM chooses appropriate state (retention vs off)
3. **IO isolation** - Prevent floating signals during power transitions
4. **Wake sources** - Configure which events can wake the system
5. **Power domain policy** - Which domains can be powered off

**Example scenario**:
```c
// Linux tells DM: "We need to resume within 50ms"
ti_sci_cmd_set_latency_constraint(handle, 50, TISCI_MSG_CONSTRAINT_SET);

// Linux tells DM: "We're suspending, you choose the best state"
ti_sci_cmd_prepare_sleep(handle, TISCI_MSG_VALUE_SLEEP_MODE_DM_MANAGED, 0, 0, 0);

// DM decides: "50ms latency means I can use retention mode but not full power-off"
// DM plans: "I'll keep PMIC in retention, DDR in self-refresh, disable most domains"

// Now PSCI triggers the actual transition
psci_system_suspend();

// ATF coordinates with DM to execute the planned transition
```

### SOC Off vs Suspend-to-RAM

#### Suspend-to-RAM (supported)
- DDR remains powered (self-refresh mode)
- CPU clusters powered off
- Core voltage domains may be retained or powered off
- Wake latency: typically 10-100ms
- Requires Device Manager support (MSG_FLAG_CAPS_LPM_DM_MANAGED)

#### SOC Off (platform-dependent)
- DDR powered off (requires context save to non-volatile storage)
- All power domains off except wake controllers and RTC
- Wake latency: 100ms-1s+
- Would require additional firmware support
- Context would need to be saved (ctx_lo/ctx_hi parameters in prepare_sleep)
- May not be supported on all TI K3 variants

### Key Files Reference

| Component | File | Key Functions/Lines |
|-----------|------|---------------------|
| Suspend core | `kernel/power/suspend.c` | `suspend_enter()`:413, `enter_state()`:570 |
| PSCI driver | `drivers/firmware/psci/psci.c` | `psci_system_suspend_enter()`:540, `psci_system_suspend()`:530, `invoke_psci_fn()`:113-132 |
| TI SCI driver | `drivers/firmware/ti_sci.c` | `ti_sci_suspend()`:3709, `ti_sci_prepare_system_suspend()`:3678, `ti_sci_cmd_prepare_sleep()`:1661, `ti_sci_suspend_noirq()`:3749, `ti_sci_resume_noirq()`:3761 |
| ARM64 suspend | `arch/arm64/kernel/suspend.c` | `cpu_suspend()`:97, `__cpu_suspend_exit()`:44 |
| ARM64 asm | `arch/arm64/kernel/sleep.S` | `__cpu_suspend_enter()`:65, `cpu_resume()`:101, `_cpu_resume()`:116 |
| Device Tree | `arch/arm64/boot/dts/ti/k3-j784s4.dtsi` | CPU nodes:58-169 |
| | `arch/arm64/boot/dts/ti/k3-j784s4-j742s2-common.dtsi` | PSCI node:55-58 |

### Key Takeaways

1. **Dual firmware architecture**: TI SCI for SoC policy, PSCI for CPU control
2. **TI SCI first, PSCI second**: Configuration before transition
3. **Device Manager autonomy**: DM selects power state based on constraints
4. **Standard ARM flow**: After TI SCI setup, follows standard ARM64 suspend path
5. **Context save importance**: CPU state saved in Linux, indexed by MPIDR
6. **Resume entry point**: `cpu_resume` physical address passed to PSCI
7. **Identity mapping critical**: Resume runs in physical mode before MMU enabled
8. **IO isolation prevents issues**: Critical for signal integrity during power transitions

Understanding this flow is essential for:
- Debugging suspend/resume failures
- Adding wake source support
- Optimizing resume latency
- Porting to new TI K3 platforms
- Implementing custom power states

---
