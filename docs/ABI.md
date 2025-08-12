# Loongson 2K1000LA VPU: Hardware Interface and User-Space ABI

> [!NOTE]
> This is AI-generated (GPT-5) from the sources. Use with caution.

This document describes the kernel module interfaces implemented by this repository (character device, ioctls, mmap, fasync/interrupts) and the minimal hardware register surface the driver exposes to user-space.

The intent is documentation only. The driver is considered a thin shim around device discovery and IRQ/memory plumbing; all device programming is expected to be performed by a vendor user-space component.

## Device node

- Character device: `/dev/gsgpu_vdec`
- Permissions: 0666 (world-readable/writable)
- Class: `gsgpu_vdec`

## Overview

On probe (PCI or platform):

- The driver maps the VPU register aperture and exposes it to user-space via `mmap(2)` (non-cached).
- It requests one interrupt and dispatches it to user-space via `SIGIO` to registered fasync listeners.
- It initializes a pre-allocated pool of physically contiguous, zeroed memory blocks of varying sizes (DMA32, up to ~7.8 MiB per block) for user-space to borrow via ioctls.

No in-kernel command submission or scheduling is implemented; user-space is expected to program hardware registers directly and use the provided IRQ and memory services.

## UAPI header

The public ABI is declared in `src/gsgpu_vdec.h`.

Key types:

- `mem_param_t { unsigned addr; unsigned size; }` — argument for allocation ioctl (in/out).
- `mem_block_t` — internal (not exposed to user-space directly).

Key constants:

- `GSGPU_VDEC_V = 0x4` and `GSGPU_VDEC_H = 0x240` — register offsets used by the ISR.
- `GSGPU_VINT = 0x100`, `GSGPU_HINT = 0x100` — interrupt bits for the V and H paths.

## Ioctls

All ioctls use magic `'L'` and are defined in `gsgpu_vdec.h`. The current max command number is `GSGPU_VDEC_IOC_MAXNR = 8`.

- `GSGPU_VDEC_INIT` — `_IO('L', 1)`
  - Purpose: Tag this file descriptor as a "host/operation" instance. Influences which fasync queue receives signals.
  - Input/Output: none
  - Returns: 0 on success, `-ENOTTY` if invalid.

- `GSGPU_VDEC_GET_TIME` — `_IO('L', 2)`
  - Purpose: Test-only. Returns the last ISR timestamp. Only enabled when the module is built with `VDEC_TEST_TIME`.
  - Input/Output: none (implementation writes directly to user pointer when enabled)
  - Notes: Not intended as stable ABI.

- `GSGPU_VDEC_IO_BASE` — `_IOR('L', 3, unsigned long *)`
  - Purpose: Retrieve the physical base address of the VPU register aperture.
  - Arg: pointer to `unsigned long` that receives the physical base.

- `GSGPU_VDEC_IO_SIZE` — `_IOR('L', 4, unsigned int *)`
  - Purpose: Retrieve the size in bytes of the VPU register aperture.
  - Arg: pointer to `unsigned int` that receives the size.

- `GSGPU_VDEC_CLI` — `_IO('L', 5)`
  - Purpose: Disable the VPU IRQ (locally) via `disable_irq()`.

- `GSGPU_VDEC_STI` — `_IO('L', 6)`
  - Purpose: Enable the VPU IRQ (locally) via `enable_irq()`.

- `GSGPU_VDEC_ALLOC` — `_IOWR('L', 7, unsigned long)`
  - Purpose: Obtain a physically contiguous, DMA32-able block from the driver's pool.
  - Arg: user supplies `mem_param_t { .size = N }`; driver fills `.addr` with the physical address of the allocated block. The `.size` field is not modified by the driver.
  - Constraints: Max request size is `BLOCK_MAX_SIZE` (currently `1999 * PAGE_SIZE`). Allocation is satisfied from a fixed-size pool; failure occurs when no free block large enough exists.
  - Lifetime: Blocks are associated with the allocating file descriptor and are released on `close(2)` automatically or via `GSGPU_VDEC_FREE`.
  - Errors: `-ENOMEM` if request exceeds cap or pool exhausted; `-EFAULT` on bad user pointer.

- `GSGPU_VDEC_FREE` — `_IOW('L', 8, unsigned long)`
  - Purpose: Return a previously allocated block to the pool.
  - Arg: pointer to `unsigned int` containing the physical base address of the block to free.
  - Errors: `0` on success; silently ignores unknown addresses.

## Memory allocation pool

- Pre-allocated at probe using `alloc_pages_exact(size, GFP_DMA32 | __GFP_ZERO)`.
- Table-driven sizes: see `mem_table[]` in `gsgpu_vdec.c` for the list of page counts; blocks range from small (1 page) to large (up to 1999 pages).
- Address returned to user-space is the physical address (bus address) of the block.
- No CPU mapping is provided by the driver; user-space is expected to use this as a device-accessible buffer address programmed into the VPU.
- On driver unload, all blocks are freed via `free_pages_exact`.

## mmap of registers

- `mmap(2)` with offset 0 maps the device register range non-cached.
- Mapping size is capped to the register aperture size reported by `GSGPU_VDEC_IO_SIZE`.
- Alternatively, an offset equal to a PFN can be supplied to map other PFNs, but typical usage is offset 0 for the VPU regs.

### Register programming contract

- The driver does not validate or virtualize register accesses. User-space must know the register map.
- ISR-related registers:
  - Offset `GSGPU_VDEC_V` (0x4): interrupt status/control; bit `GSGPU_VINT` (0x100) indicates/clears V interrupt.
  - Offset `GSGPU_VDEC_H` (0x240): interrupt status/control; bit `GSGPU_HINT` (0x100) indicates/clears H interrupt.
- `HwInit()` zeros the entire register aperture at probe.

## Interrupts and async notification

- One shared IRQ is requested on the device.
- ISR (`gsgpu_vdec_isr`) reads `GSGPU_VDEC_V` and `GSGPU_VDEC_H`. If the respective bit is set, it clears it by writing back the register value with the bit masked out.
- User-space can receive `SIGIO` upon interrupts by enabling fasync on the fd (e.g., via `fcntl(fd, F_SETFL, O_ASYNC)` and setting ownership `F_SETOWN`).
  - There are two async queues: `aqv` and `aqh`, corresponding to the two interrupt sources; which queue your fd is attached to depends on whether you called `GSGPU_VDEC_INIT` on that fd.

## Open/close semantics

- `open(2)`: returns a handle; `private_data` is set to identify whether the fd is an operation instance (`GSGPU_VDEC_INIT` called) or a default instance.
- `release(2)`: disables fasync for the fd, then releases all memory pool allocations associated with the fd.

## PCI vs Platform

- Platform compatible string: `loongson,ls-vpu`
- PCI ID: vendor `PCI_VENDOR_ID_LOONGSON`, device `0x7a16`
- In both cases, the same character device and ABI are exposed.

## Versioning

- Driver prints `vdec driver date: <YYYYMMDD>` at init (see `VDEC_DRIVER_DATE`).
- No formal uAPI versioning is provided; ABI is stable as described here until code changes.

## Minimal user-space usage sketch

- Discover and open the device `/dev/gsgpu_vdec`.
- Query base/size via ioctls; `mmap` the register window and program the VPU.
- Allocate one or more physical buffers via `GSGPU_VDEC_ALLOC` and program their physical addresses into the VPU descriptors.
- Enable async notifications and handle `SIGIO` to be informed of hardware events; or poll the status registers through the mapped aperture.
- Free buffers with `GSGPU_VDEC_FREE` or by closing the fd.

## Notes and caveats

- No cache management is performed by the driver for the allocated buffers. If the CPU accesses the memory, user-space must ensure appropriate cache maintenance relative to the device (platform-dependent).
- No IOMMU support is implemented; addresses are raw physical (DMA32) addresses.
- The allocation pool is global and fixed-size; concurrent processes compete for blocks.
- The `GSGPU_VDEC_GET_TIME` ioctl is only compiled in with `VDEC_TEST_TIME` and should not be relied upon.
