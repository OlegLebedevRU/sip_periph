# sip_periph

STM32F4 firmware project for `STM32F411CEU` built with STM32CubeIDE/HAL/FreeRTOS.

## What is in the repository

- `Core/` — application sources and headers
- `Drivers/` — STM32 HAL and CMSIS
- `Middlewares/` — middleware components
- `USB_DEVICE/` — USB device stack
- `docs/` — project notes and integration docs
- `sip_periph.ioc` — STM32CubeMX project file
- `STM32F411CEUX_FLASH.ld`, `STM32F411CEUX_RAM.ld` — linker scripts

## GitHub publication status

This folder is prepared for GitHub publication with a typical STM32 `.gitignore`.
Build artifacts such as `Debug/`, `Release/`, `*.elf`, `*.bin`, `*.hex`, `*.map` and local IDE settings are excluded.

## Publish to GitHub from Windows `cmd.exe`

1. Create an empty repository on GitHub.
2. Open `cmd.exe` in this folder.
3. Run:

```bat
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/<YOUR_USERNAME>/<YOUR_REPO>.git
git push -u origin main
```

## Notes

- Shared STM32CubeIDE project files such as `.project`, `.cproject` and `.ioc` are kept in version control.
- Local workspace/build output folders are ignored.
- If you want to publish firmware binaries, prefer GitHub Releases instead of committing files from `Debug/` or `Release/`.
