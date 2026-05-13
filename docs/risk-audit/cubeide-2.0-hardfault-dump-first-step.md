# Первый шаг: как понять причину HardFault на текущей прошивке через ST-LINK и STM32CubeIDE 2.0

Цель этого шага — **ничего не менять в коде**, а поймать текущее падение «как есть сейчас», остановиться в `HardFault_Handler`, сохранить регистры и стековый frame.

## Что нужно получить

После одного воспроизведения нужно сохранить:

- `R0-R3`
- `R12`
- `LR`
- `PC`
- `xPSR`
- `MSP`
- `PSP`
- `SCB->CFSR`
- `SCB->HFSR`
- `SCB->MMFAR`
- `SCB->BFAR`
- `SCB->SHCSR`
- 64-128 байт памяти по адресу активного stack pointer

Это уже обычно позволяет понять:

- invalid pointer / выход за границы памяти;
- unaligned access;
- divide by zero;
- вызов FreeRTOS API из ISR с недопустимым приоритетом;
- return в битый адрес;
- stack corruption.

## Порядок запуска в STM32CubeIDE 2.0

### 1. Собрать debug-конфигурацию

1. Открыть проект `<project_root>` в STM32CubeIDE 2.0.
2. Выбрать конфигурацию **Debug**.
3. Убедиться, что компиляция идёт с debug symbols (`-g`).
4. Выполнить **Project -> Build Project**.

### 2. Создать debug-конфигурацию под ST-LINK

1. Открыть **Run -> Debug Configurations...**.
2. Выбрать **STM32 Cortex-M C/C++ Application**.
3. Создать/открыть конфигурацию проекта `sip_periph`.
4. Во вкладке **Debugger**:
   - Interface: **SWD**
   - Reset mode: **Hardware reset** или **Connect under reset**, если MCU иногда уходит в fault до attach
   - Включить **Halt on startup**
5. Применить настройки.

### 3. Поставить breakpoint на fault handlers

Перед запуском поставить breakpoint на:

- `HardFault_Handler`
- `MemManage_Handler`
- `BusFault_Handler`
- `UsageFault_Handler`

Файл:
- `Core/Src/stm32f4xx_it.c`

Если доступен breakpoint manager, лучше сразу добавить все четыре.

### 4. Запустить debug-сессию

1. Нажать **Debug**.
2. Дождаться остановки на `main()`.
3. Нажать **Resume**.
4. Воспроизвести сценарий, который приводит к зависанию/HardFault.

### 5. Когда исполнение остановится в fault handler

Сразу сохранить состояние, не делая Step/Resume.

#### Сохранить регистры

Открыть:
- **Window -> Show View -> Registers**
- **Window -> Show View -> Expressions**
- **Window -> Show View -> Memory**

Записать значения:

- Core registers: `r0-r12`, `sp`, `lr`, `pc`, `xpsr`
- Если видны отдельно special registers: `msp`, `psp`, `primask`, `basepri`, `faultmask`

#### Сохранить fault-регистры через Expressions

Добавить выражения:

- `SCB->CFSR`
- `SCB->HFSR`
- `SCB->MMFAR`
- `SCB->BFAR`
- `SCB->SHCSR`
- `SCB->ICSR`

#### Определить активный стек

В окне **Registers** посмотреть `LR` (EXC_RETURN):

- если `LR & 4 == 0` -> fault frame лежит в **MSP**
- если `LR & 4 != 0` -> fault frame лежит в **PSP**

Использовать соответствующий указатель как `fault_sp`.

### 6. Считать стековый frame вручную

Открыть **Memory** по адресу `fault_sp`.

Первые 8 слов frame на Cortex-M:

- `[0]` = `R0`
- `[1]` = `R1`
- `[2]` = `R2`
- `[3]` = `R3`
- `[4]` = `R12`
- `[5]` = `LR` (return address before exception return)
- `[6]` = `PC`
- `[7]` = `xPSR`

Сохранить минимум 32 байта, лучше 64-128 байт вокруг `fault_sp`.

### 7. Сохранить дамп

Минимально сохранить в текстовый файл:

- дату/время;
- git commit/branch;
- сценарий воспроизведения;
- все регистры выше;
- active stack pointer;
- 8 слов fault frame;
- SCB fault-регистры;
- строку исходника, на которой остановился `PC`.

Практически в CubeIDE удобнее сделать так:

1. Screenshot окна Registers.
2. Screenshot окна Expressions.
3. Export/copy Memory view для `fault_sp`.
4. Скопировать Call Stack.
5. Сохранить всё рядом в папку инцидента на ПК.

## Как быстро интерпретировать

### `CFSR`

Смотреть биты трёх подполей:

- `MMFSR` (Memory Management)
- `BFSR` (Bus Fault)
- `UFSR` (Usage Fault)

Типовые признаки:

- `PRECISERR` -> точная ошибка доступа по адресу, часто битый указатель
- `IBUSERR` -> попытка исполнять код из невалидного адреса
- `UNDEFINSTR` -> переход в мусор / повреждённый PC
- `INVSTATE` / `INVPC` -> сломан exception return или повреждён стек
- `UNALIGNED` -> unaligned access
- `DIVBYZERO` -> деление на ноль

### `BFARVALID` / `MMARVALID`

Если выставлены, адрес в `BFAR/MMFAR` обычно очень полезен: это реальный faulting address.

### `PC`

Это главный указатель на место, где CPU упал.
Если `PC` указывает в:

- код проекта -> искать bug в конкретной функции;
- FreeRTOS port/assert -> проверять ISR priority / API misuse;
- странный адрес вне flash/ram -> вероятно stack corruption или битый function pointer.

## Рекомендуемый сценарий первого шага

1. Сначала поймать падение **без перепрошивки с диагностическими изменениями**.
2. Сохранить один «сырой» дамп через debugger.
3. Только после этого переходить к диагностическому варианту fault handler с coredump в `.noinit`.

Это важно, чтобы не замаскировать исходную проблему новым кодом.

## Что сделать вторым шагом

После сохранения сырого дампа уже имеет смысл внедрять диагностический fault handler, который:

- сам сохраняет frame в `.noinit`;
- пишет `CFSR/HFSR/MMFAR/BFAR`;
- делает `NVIC_SystemReset()`;
- на следующем старте печатает причину последнего crash.

Именно этот второй шаг подготовлен в файлах:

- `docs/risk-audit/proposed/Core/Src/fault_capture_example.c`
- `docs/risk-audit/proposed/patches/linker-noinit-snippet.ld`
