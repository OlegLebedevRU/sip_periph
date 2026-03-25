# App-layer DI concept

## Scope
- DI применяется только в `app_*` слое.
- `service_*` на этом шаге не получают новых обязательных DI-точек.
- `main` постепенно сводится к bootstrap и lifecycle-вызовам `app_bootstrap`.

## Core objects
- `app_context_t` — прикладной контейнер переходного этапа.
- Сейчас он хранит:
  - `runtime_config_t runtime_config`
  - `uint8_t *i2c_ram`
- Публичный API объявлен в `Core/Inc/service_runtime_config.h`.

## Ownership and lifecycle
- Базовый путь: `main`/`app_bootstrap` получает активный `app_context` через `app_context_get()`.
- Для DI-friendly сценария app-слой может передать внешний `app_context_t` через `app_context_set_active()`.
- `app_bootstrap_pre_init()` фиксирует активный контекст.
- `app_bootstrap_wire()` связывает контекст с ресурсами bootstrap, в том числе с `i2c_ram`.
- `app_bootstrap_start()` запускает app/service init в существующем порядке старта.

## Transitional singleton fallback
- Singleton fallback разрешен только на переходный период.
- Текущий fallback реализован через внутренний default-context в `service_runtime_config.c`.
- Старый API `runtime_config_get()` остается совместимым и читает конфигурацию из активного контекста.
- Удаление fallback выполняется после стабилизации app-layer API.

## Stabilization criterion
- Критерий стабилизации: отсутствуют изменения сигнатур app-layer API.
- После достижения критерия следующий отдельный шаг — удалить singleton fallback и оставить только явный app-context flow.

## Rules
- Новые зависимости в `app_*` модулях передаются через `app_context`, без добавления новых глобалов.
- Fixed hooks `app_bootstrap_pre_init()`, `app_bootstrap_wire()`, `app_bootstrap_start()` являются точками orchestration.
- Ограничения на содержимое hooks сейчас намеренно не вводятся; их поведение контролируется по месту вызова и через поэтапный рефакторинг.

## Current bootstrap usage
- `app_bootstrap_start()` now owns one-time startup for `service_relay_actuator`, `service_matrix_kbd`, `app_i2c_slave`, and `service_tca6408`.
- TCA6408 bootstrap (`service_tca6408_init()` / `service_tca6408_post_bootstrap()`) is no longer started directly from `main` task code.
- App-layer consumers should prefer `app_context_i2c_ram(...)` over direct raw getters when wiring bootstrap-time RAM access.

## Transitional compatibility
- `app_i2c_slave_get_ram()` remains available as a compatibility path during migration.
- New wiring should prefer context-bound RAM first and only use the raw getter as fallback where migration is still incomplete.