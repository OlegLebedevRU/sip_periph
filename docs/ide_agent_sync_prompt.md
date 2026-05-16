# IDE Agent Prompt: Sync Run/Debug + CubeMX `.ioc`

Этот промт предназначен для **ручного запуска агента в IDE** после получения обновлений из репозитория.

## Цель

Синхронизировать IDE-конфигурации проекта с актуальным состоянием репозитория:
- run/debug конфигурации,
- `.project` / `.cproject`,
- `sip_periph.ioc` / `.mxproject`.

## Prompt для агента

```text
You are working in repository OlegLebedevRU/sip_periph.

Task: synchronize IDE project configuration after pulling latest changes.

Requirements:
1) Treat sip_periph.ioc and .mxproject as source-of-truth for CubeMX configuration.
2) Open/refresh STM32CubeIDE project metadata (.project, .cproject) and ensure run/debug launch settings are consistent with current build artifacts and project paths.
3) Do not modify application logic files unless required by CubeMX regeneration side effects.
4) Keep all generated-code safety rules:
   - do not edit generated files outside USER CODE blocks.
5) Verify build configuration references are valid for current project layout:
   - Debug/Release output paths
   - ELF path used by debug launcher
   - ST-Link/OpenOCD launch target points to this project
6) If CubeMX regeneration is needed, regenerate and report exactly which generated files changed.
7) Provide a short report:
   - what was synchronized
   - which config files changed
   - whether manual IDE action is still required.
```

## Когда запускать

- После `git pull`/merge в локальную ветку.
- После изменений в `sip_periph.ioc`, `.mxproject`, `.project`, `.cproject`.
- После конфликтов/переноса workspace, когда run/debug профили в IDE перестали совпадать с проектом.
