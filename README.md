# loongvpu-kernel-dkms

DKMS sources for the Loongson 2K3000's VPU. The SPDX license identifiers in the source files indicate that the module is GPLv2-licensed.

Note: the driver implements a different API (DRM + private ioctls) than the Hantro support present in mainline Linux (V4L2 Request API). Hence, the driver is useless without closed-source userspace blobs, and only serves as a reference regarding the hardware/firmware interface.
