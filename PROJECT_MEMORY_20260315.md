# Project Memory

This file captures the actionable state for `/root/ClickHouse-wasm` so the work can be resumed on the remote server without reconstructing the full chat history.

## Local Project Changes

- Added and updated analysis and change-tracking documents:
  - `BUR_CLICKHOUSE_FIO_HFT_ANALYSIS.md`
  - `CODE_CHANGE_NOTES_20260313.md`
  - `CLAUDE.md`
- Reworked `clickhouse_zerocopy_v2.c` to make the v2 preload library usable as a standalone `LD_PRELOAD` library.
- Updated `Makefile.v2` and `Makefile.v3` link order.
- Fixed and expanded `test_zerocopy_v2.c`.
- Added:
  - `run_fio_zerocopy_v2.sh`
  - `fio_zerocopy_v2.fio`
  - `synthetic_hft.c`
  - `run_synthetic_hft_zerocopy_v2.sh`

## Git

- Local commit created for the code/doc changes:
  - `712ad3d5cc2`
  - Message: `修正 zerocopy v2 preload 并补齐 fio/HFT 验证工具`

## Remote Server

- Host: `hyp@40.124.82.128`
- SSH key used locally: `bur_azure_key.pem`
- Project synced to:
  - `~/ClickHouse-wasm`

## Remote Build State

- User-space dependencies were installed on the remote Ubuntu host.
- Root project targets were built remotely.
- `UBR` kernel was built remotely with out-of-tree output:
  - Build dir: `/mnt/nvme0/ubr-build`
- Build completed successfully with exit code `0`.
- Built kernel release:
  - `6.16.0-rc7-ubr+`

## Archived Remote Install Set

Because `/mnt/nvme0` is ephemeral, an installable archive set was copied to the persistent root disk:

- `/home/hyp/ubr-artifacts/ubr-install-files.tar.zst`
- `/home/hyp/ubr-artifacts/ubr-install-files.txt`
- `/home/hyp/ubr-artifacts/ubr-install-files.sha256`
- `/home/hyp/ubr-artifacts/INSTALL_INFO.txt`
- `/home/hyp/ubr-artifacts/INSTALL_FROM_ARCHIVE.md`

This archive contains enough files to install the already-built kernel without recompiling:

- `arch/x86/boot/bzImage`
- all compiled `*.ko` modules
- `modules.order`
- `modules.builtin`
- `modules.builtin.modinfo`
- `modules.builtin.ranges`
- `System.map`
- `.config`
- `include/config/kernel.release`
- `Module.symvers`

Not included:

- `vmlinux` (not required for install/boot, only for debugging/symbol analysis)

## Remote Installation State

The archived kernel has already been installed on the remote server.

Installed locations:

- `/lib/modules/6.16.0-rc7-ubr+/`
- `/boot/vmlinuz-6.16.0-rc7-ubr+`
- `/boot/initrd.img-6.16.0-rc7-ubr+`
- `/boot/System.map-6.16.0-rc7-ubr+`
- `/boot/config-6.16.0-rc7-ubr+`

Validation observed after install:

- Module count under `/lib/modules/6.16.0-rc7-ubr+`:
  - `2971`
- `saved_entry` in grub environment:
  - `Advanced options for Ubuntu>Ubuntu, with Linux 6.16.0-rc7-ubr+`
- `/etc/default/grub` was changed to:
  - `GRUB_DEFAULT=saved`
- Backup made:
  - `/etc/default/grub.bak_20260315_ubr`

Current remote running kernel:

- `6.16.0-rc7-ubr+`

Observed follow-up state:

- The remote host has already rebooted into the installed UBR kernel.
- The archived install set in `/home/hyp/ubr-artifacts/` remains the persistent source of truth.

## Important Constraints

- The remote install was done from the archive, not by rerunning kernel compilation.
- If work resumes later, do not assume `/mnt/nvme0/ubr-build` persists across VM stop/start.
- The persistent source of truth for installation is `/home/hyp/ubr-artifacts/`.
