# AMD CLR - Compute Language Runtimes

AMD Common Language Runtime contains source code for AMD's compute languages runtimes: `HIP` and `OpenCL™`.

## Project Organisation

- `hipamd` - contains implementation of `HIP` language on AMD platform. Previously this was hosted at [ROCm-Developer-Tools/hipamd](https://github.com/ROCm-Developer-Tools/hipamd)
- `opencl` - contains implementation of [OpenCL™](https://www.khronos.org/opencl/) on AMD platform. Previously this was hosted at [RadeonOpenCompute/ROCm-OpenCL-Runtime](https://github.com/RadeonOpenCompute/ROCm-OpenCL-Runtime)
- `rocclr` - contains common runtime used in `HIP` and `OpenCL™`. Previously this was hosted at [ROCm-Developer-Tools/ROCclr](https://github.com/ROCm-Developer-Tools/ROCclr)

## How to build/install

### Prerequisites

Please refer to Quick Start Guide in [ROCm Docs](https://rocm.docs.amd.com/projects/install-on-linux/en/latest/tutorial/quick-start.html#rocm-install-quick).

Building clr requires `rocm-hip-libraries` meta package, which provides the pre-requisites for clr.

### Linux

- Clone this repo
- `cd clr && mkdir build && cd build`
- For HIP : `cmake .. -DCLR_BUILD_HIP=ON -DHIP_COMMON_DIR=$HIP_COMMON_DIR`
  - `HIP_COMMON_DIR` points to [HIP](https://github.com/ROCm/HIP)
  - `HIPCC_BIN_DIR` points to [HIPCC](https://github.com/ROCm/HIPCC)'s bin folder. If not provided, it defaults to `/opt/rocm/bin`.
- For OpenCL™ : `cmake .. -DCLR_BUILD_OCL=ON`
- `make` : to build
- `make install` : to install

Users can also build `OCL` and `HIP` at the same time by passing `-DCLR_BUILD_HIP=ON -DCLR_BUILD_OCL=ON` to configure command.

## Tests

`hip-tests` is a separate repo hosted at [hip-tests](https://github.com/ROCm/hip-tests).

To run `hip-tests` please go to the repo and follow the steps.

There used to be directed tests in [ROCm/HIP](https://github.com/ROCm/HIP) which are now removed in favor of `hip-tests` repo.

## Disclaimer

The information presented in this document is for informational purposes only and may contain technical inaccuracies, omissions, and typographical errors. The information contained herein is subject to change and may be rendered inaccurate for many reasons, including but not limited to product and roadmap changes, component and motherboard versionchanges, new model and/or product releases, product differences between differing manufacturers, software changes, BIOS flashes, firmware upgrades, or the like. Any computer system has risks of security vulnerabilities that cannot be completely prevented or mitigated.AMD assumes no obligation to update or otherwise correct or revise this information. However, AMD reserves the right to revise this information and to make changes from time to time to the content hereof without obligation of AMD to notify any person of such revisions or changes.THIS INFORMATION IS PROVIDED ‘AS IS.” AMD MAKES NO REPRESENTATIONS OR WARRANTIES WITH RESPECT TO THE CONTENTS HEREOF AND ASSUMES NO RESPONSIBILITY FOR ANY INACCURACIES, ERRORS, OR OMISSIONS THAT MAY APPEAR IN THIS INFORMATION. AMD SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR ANY PARTICULAR PURPOSE. IN NO EVENT WILL AMD BE LIABLE TO ANY PERSON FOR ANY RELIANCE, DIRECT, INDIRECT, SPECIAL, OR OTHER CONSEQUENTIAL DAMAGES ARISING FROM THE USE OF ANY INFORMATION CONTAINED HEREIN, EVEN IF AMD IS EXPRESSLY ADVISED OF THE POSSIBILITY OF SUCH DAMAGES. AMD, the AMD Arrow logo, and combinations thereof are trademarks of Advanced Micro Devices, Inc. Other product names used in this publication are for identification purposes only and may be trademarks of their respective companies.

© 2023 Advanced Micro Devices, Inc. All Rights Reserved.

OpenCL™ is registered Trademark of Apple
