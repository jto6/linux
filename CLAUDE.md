# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is the Linux kernel source tree. For comprehensive codebase architecture and structure, **refer to `code_analysis.md`** which contains detailed analysis of subsystems, design patterns, data flows, and investigation prompts.

## Essential Build Commands

### Configuration
```bash
# Interactive configuration (ncurses menu)
make menuconfig

# Use default configuration for current architecture
make defconfig

# Minimal config for currently loaded modules
make localmodconfig

# Update existing .config with new options
make oldconfig

# Check if Rust toolchain is available
make rustavailable
```

### Compilation
```bash
# Full kernel build (parallel)
make -j$(nproc)

# Build only modules
make modules

# Build device tree blobs (ARM/embedded)
make dtbs

# Build specific subsystem (e.g., drivers/net)
make drivers/net/

# Build single module
make M=drivers/example/

# Build with additional warnings
make W=1

# Build with Clang instead of GCC
make CC=clang
```

### Testing and Validation
```bash
# Run checkpatch on changes
scripts/checkpatch.pl --file path/to/file.c
scripts/checkpatch.pl --git HEAD~1..HEAD

# Static analysis with sparse
make C=1          # Check files being recompiled
make C=2          # Check all source files

# Run KUnit tests
./tools/testing/kunit/kunit.py run

# Run kernel selftests
make -C tools/testing/selftests
make -C tools/testing/selftests run_tests

# Build documentation
make htmldocs
make pdfdocs
```

### Installation
```bash
# Install modules to /lib/modules/$(uname -r)
sudo make modules_install

# Install kernel (distribution-specific)
sudo make install

# Clean build artifacts
make clean        # Keep .config
make mrproper     # Remove all generated files including .config
```

### Code Navigation
```bash
# Generate tags for editors
make tags         # Emacs/vi tags
make cscope       # Cscope database
make gtags        # GNU GLOBAL tags

# View available make targets
make help
```

## Architecture and Big Picture

### Kernel Boot and Initialization Flow

The kernel boot sequence follows a specific path from bootloader to userspace:

1. **Architecture Entry** (`arch/*/boot/`) - Bootloader hands off control
2. **`start_kernel()`** (`init/main.c:910`) - Main initialization routine that:
   - Initializes memory management (`mm_core_init()`)
   - Sets up the scheduler (`sched_init()`)
   - Initializes interrupt handling (`init_IRQ()`)
   - Calls architecture-specific setup (`setup_arch()`)
3. **`rest_init()`** (`init/main.c:711`) - Spawns kernel threads
4. **`kernel_init()`** (`init/main.c:1474`) - Finalizes initialization
5. **`do_initcalls()`** (`init/main.c:1348`) - Runs module init functions in priority order
6. **First userspace process** - Executes `/sbin/init` or initramfs init

### System Call Path

User applications interact with kernel through system calls:

```
User Space: syscall(SYS_read, ...)
    ↓ (syscall instruction)
arch/*/entry.S: entry_SYSCALL_64()
    ↓
arch/*/kernel/syscall.c: Dispatch via sys_call_table
    ↓
Kernel Handler: sys_read() → ksys_read() → vfs_read()
    ↓
Return to User Space
```

### Memory Management Layers

The kernel uses a tiered memory allocation strategy:

- **Physical Pages**: Buddy allocator (`mm/page_alloc.c`) manages pages in power-of-2 chunks
- **Object Caching**: SLUB allocator (`mm/slub.c`) provides fast per-CPU object caches
- **Virtual Memory**: Process address spaces managed by VMA structures (`mm/mmap.c`)
- **Page Cache**: File data cached in memory (`mm/filemap.c`)

Key insight: Most allocations use `kmalloc()` (backed by SLUB) which internally uses buddy allocator for slabs.

### VFS Layering and Filesystem Integration

The Virtual Filesystem Switch (VFS) provides abstraction over specific filesystems:

```
System Call (open/read/write) → VFS Layer (fs/)
    ↓
Generic File Operations (struct file_operations)
    ↓
Filesystem-Specific Implementation (fs/ext4/, fs/btrfs/, etc.)
    ↓
Block Layer (block/) OR Page Cache (mm/filemap.c)
    ↓
Device Drivers (drivers/block/)
```

Each filesystem registers function pointers for operations (open, read, write, etc.) that VFS calls. See `struct file_operations` and `struct address_space_operations` in `include/linux/fs.h`.

### Process Scheduler Architecture

The Completely Fair Scheduler (CFS) in `kernel/sched/fair.c` uses a red-black tree to track runnable tasks:

- Each task has a `vruntime` (virtual runtime) tracking CPU time used
- Tasks are ordered in rbtree by vruntime
- Scheduler always picks leftmost task (smallest vruntime)
- Per-CPU runqueues minimize lock contention
- Load balancing periodically moves tasks between CPUs

Alternative schedulers (RT, deadline) coexist for real-time workloads.

### Network Stack Flow

Packet processing follows a layered architecture:

**Receive Path**:
```
NIC Hardware → DMA to memory
    ↓
Hardware IRQ → Driver rx_handler (drivers/net/)
    ↓
NAPI softirq (net/core/dev.c:netif_receive_skb)
    ↓
Protocol handlers (net/ipv4/ip_input.c)
    ↓
Transport layer (net/ipv4/tcp_ipv4.c)
    ↓
Socket buffer → Application
```

**Transmit Path**:
```
Application → sys_send*/sys_write
    ↓
Socket layer (net/socket.c)
    ↓
Transport layer (net/ipv4/tcp.c:tcp_sendmsg)
    ↓
IP layer (net/ipv4/ip_output.c)
    ↓
Device layer (net/core/dev.c:dev_queue_xmit)
    ↓
Driver tx_handler → NIC
```

Key performance features: NAPI poll mode, TSO/GSO, XDP for fast packet processing.

### Device Driver Model

The unified device model (`drivers/base/`) provides automatic device-driver binding:

```
struct bus_type (PCI, USB, platform, etc.)
    ├─ Devices (struct device)
    └─ Drivers (struct device_driver)
```

When a device is registered, the bus type matches it to a driver (by vendor/device ID or compatible strings), then calls the driver's `probe()` function. All devices appear in sysfs hierarchy under `/sys/devices/`.

### Interrupt Handling Strategy

Two-phase interrupt handling minimizes interrupt latency:

1. **Hard IRQ** (interrupt context, non-maskable): Minimal work, acknowledge interrupt, schedule softirq
2. **Softirq** (`kernel/softirq.c`): Deferred processing in interrupt context (NET_RX_SOFTIRQ, BLOCK_SOFTIRQ, etc.)
3. **Workqueues** (`kernel/workqueue.c`): Deferred work in process context (can sleep)

This design keeps interrupt-disabled time minimal while deferring bulk work.

## Coding Style Requirements

The kernel has strict coding style rules enforced by `scripts/checkpatch.pl`:

- **Indentation**: 8-character tabs (not spaces)
- **Line length**: Prefer 80 columns (some flexibility for readability)
- **Braces**: K&R style (opening brace on same line, except functions)
- **Naming**: lowercase with underscores (`do_something_useful`)
- **Goto for cleanup**: Centralized error handling using goto labels

Critical style points:
- Switch cases align with switch statement (no double-indent)
- Always use braces for multi-statement blocks
- Never break user-visible strings (breaks grep)
- Use `fallthrough;` for intentional switch fallthrough

**Always run `scripts/checkpatch.pl` before submitting patches.**

## Module Development Workflow

### Creating a Kernel Module

Modules live in subsystem directories (e.g., `drivers/`, `fs/`, `net/`). Basic structure:

```c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init my_module_init(void)
{
    pr_info("Module loaded\n");
    return 0;
}

static void __exit my_module_exit(void)
{
    pr_info("Module unloaded\n");
}

module_init(my_module_init);
module_exit(my_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Module description");
```

### Building and Testing a Single Module

```bash
# Build specific module directory
make M=drivers/yourmodule/

# Load module
sudo insmod drivers/yourmodule/yourmodule.ko

# View kernel log
dmesg | tail

# List loaded modules
lsmod | grep yourmodule

# Unload module
sudo rmmod yourmodule

# Load with modprobe (handles dependencies)
sudo modprobe yourmodule
```

### Module Kbuild Integration

Add to parent directory's `Kconfig`:
```
config YOUR_DRIVER
    tristate "Your driver description"
    depends on DEPENDENCY
    help
      Detailed help text
```

Add to parent directory's `Makefile`:
```
obj-$(CONFIG_YOUR_DRIVER) += yourmodule.o
```

## Cross-Architecture Considerations

When modifying code that affects multiple architectures:

- **Architecture-specific code**: Goes in `arch/*/`
- **Generic code**: Use `CONFIG_*` guards to handle arch differences
- **Endianness**: Use `cpu_to_le32()`, `le32_to_cpu()` etc. for portability
- **Atomic operations**: Use `<linux/atomic.h>` abstractions
- **Memory barriers**: Use `smp_rmb()`, `smp_wmb()`, `smp_mb()` for SMP safety

Test on multiple architectures when possible (QEMU can emulate most).

## Synchronization Patterns

Understanding when to use each synchronization primitive:

- **Spinlocks** (`spinlock_t`): Short critical sections, atomic context, cannot sleep
- **Mutexes** (`struct mutex`): Longer critical sections, process context, can sleep
- **RWLocks**: Reader-writer locks (deprecated in favor of RCU for read-heavy)
- **RCU** (`rcu_read_lock()`): Lock-free for read-mostly data structures
- **Atomic operations** (`atomic_t`): Simple counters and flags without locks
- **Per-CPU variables** (`DEFINE_PER_CPU`): Avoid locking by CPU-local data

**Rule**: Never sleep while holding a spinlock or in atomic context.

## Error Handling Conventions

Kernel functions return errors as negative errno values:

```c
int some_function(void)
{
    if (error_condition)
        return -EINVAL;  // Negative errno

    return 0;  // Success
}
```

For pointers, use `ERR_PTR()` and check with `IS_ERR()`:

```c
struct foo *ptr = some_func();
if (IS_ERR(ptr))
    return PTR_ERR(ptr);  // Extract errno
```

Cleanup pattern using goto:

```c
int func(void)
{
    buf = kmalloc(...);
    if (!buf)
        goto err_alloc;

    lock = obtain_lock();
    if (!lock)
        goto err_lock;

    // Success path
    return 0;

err_lock:
    kfree(buf);
err_alloc:
    return -ENOMEM;
}
```

## Memory Allocation Guidelines

Choose the right allocator:

- **`kmalloc(size, GFP_KERNEL)`**: General kernel memory (can sleep)
- **`kmalloc(size, GFP_ATOMIC)`**: Atomic context (interrupt handlers)
- **`kzalloc()`**: Zero-initialized allocation
- **`vmalloc()`**: Large allocations (virtually contiguous, not physically)
- **`alloc_pages()`**: Allocate physical pages directly
- **`kmem_cache_create()`**: Frequently allocated objects (create custom cache)

Always check return value and handle OOM (NULL return).

## Configuration System (Kconfig)

The kernel uses Kconfig for build configuration:

- **bool**: Built-in or disabled
- **tristate**: Built-in (y), module (m), or disabled (n)
- **depends on**: Dependency specification
- **select**: Force-enable another option

Example:
```
config MY_FEATURE
    tristate "My feature support"
    depends on PCI && NET
    select CRC32
    help
      This enables my feature...
```

## Important Subsystem Entry Points

When working in specific subsystems, know the entry points:

- **Filesystems**: Register with `register_filesystem()`, implement `file_operations`
- **Network protocols**: Register with `proto_register()`, implement `proto_ops`
- **Block drivers**: Register with `register_blkdev()`, implement `block_device_operations`
- **Character drivers**: Register with `register_chrdev()`, implement `file_operations`
- **PCI drivers**: Use `pci_register_driver()`, implement `pci_driver`
- **Platform drivers**: Use `platform_driver_register()`

## Debugging Techniques

### printk and Logging
```c
pr_err("Error message\n");       // KERN_ERR level
pr_warn("Warning message\n");    // KERN_WARNING
pr_info("Info message\n");       // KERN_INFO
pr_debug("Debug message\n");     // KERN_DEBUG (needs DEBUG defined)

// Device-specific logging
dev_err(&dev->dev, "Device error\n");
```

View with `dmesg` or `/var/log/kern.log`.

### Dynamic Debug
```bash
# Enable debug messages for specific file
echo 'file drivers/mydriver.c +p' > /sys/kernel/debug/dynamic_debug/control
```

### Kernel Debuggers
- **KGDB**: GDB over serial/network
- **crash**: Post-mortem analysis of kernel dumps
- **ftrace**: Function call tracing

### Memory Debugging
- **KASAN** (`CONFIG_KASAN`): Detect memory corruption
- **UBSAN** (`CONFIG_UBSAN`): Detect undefined behavior
- **kmemleak** (`CONFIG_DEBUG_KMEMLEAK`): Find memory leaks

## Reference Documentation

Essential documentation paths (see `Documentation/` directory):

- **process/submitting-patches.rst**: How to submit kernel patches
- **process/coding-style.rst**: Official coding style guide
- **process/changes.rst**: Required tool versions and dependencies
- **core-api/**: Kernel core API documentation
- **driver-api/**: Device driver APIs
- **kbuild/**: Build system documentation
- **admin-guide/**: Kernel parameters and administration

Online: https://www.kernel.org/doc/html/latest/

## Key Files to Reference

- **MAINTAINERS**: Find subsystem maintainers and mailing lists
- **code_analysis.md**: Comprehensive codebase architecture analysis (created by Claude Code)
- **.config**: Current kernel build configuration
- **System.map**: Kernel symbol addresses (generated after build)

## Environment Variables for Build Customization

- **ARCH**: Target architecture (e.g., `ARCH=arm64`)
- **CROSS_COMPILE**: Cross-compiler prefix (e.g., `CROSS_COMPILE=aarch64-linux-gnu-`)
- **KCFLAGS**: Additional C compiler flags
- **KRUSTFLAGS**: Additional Rust compiler flags
- **V=1**: Verbose build output

Example cross-compilation:
```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
```

## Rust Support (Experimental)

The kernel has experimental Rust support for driver development:

```bash
# Check if Rust toolchain is available
make rustavailable

# Build with Rust enabled (requires CONFIG_RUST=y)
make
```

Rust code lives in `rust/` and uses bindings generated from C headers. See `Documentation/rust/` for details.
