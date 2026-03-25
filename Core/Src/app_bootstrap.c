#include "app_bootstrap.h"

#include <stddef.h>

#include "app_i2c_slave.h"
#include "service_matrix_kbd.h"
#include "service_relay_actuator.h"
#include "service_tca6408.h"

static app_context_t *s_bootstrap_ctx = NULL;
static uint8_t *s_bootstrap_ram = NULL;

void app_bootstrap_pre_init(const app_bootstrap_init_t *init)
{
    if (init == NULL) {
        s_bootstrap_ctx = app_context_get();
        s_bootstrap_ram = NULL;
        return;
    }

    s_bootstrap_ctx = (init->context != NULL) ? init->context : app_context_get();
    s_bootstrap_ram = init->i2c_ram;

    if (s_bootstrap_ctx != NULL) {
        app_context_set_active(s_bootstrap_ctx);
    }
}

void app_bootstrap_wire(void)
{
    if (s_bootstrap_ctx == NULL) {
        s_bootstrap_ctx = app_context_get();
    }

    if ((s_bootstrap_ctx != NULL) && (s_bootstrap_ram != NULL)) {
        app_context_bind_i2c_ram(s_bootstrap_ctx, s_bootstrap_ram);
    }
}

void app_bootstrap_start(void)
{
    uint8_t *i2c_ram = NULL;

    service_relay_actuator_init();
    service_matrix_kbd_init();
    app_i2c_slave_init();

    if (s_bootstrap_ctx == NULL) {
        s_bootstrap_ctx = app_context_get();
    }

    i2c_ram = app_context_i2c_ram(s_bootstrap_ctx);
    if (i2c_ram == NULL) {
        i2c_ram = s_bootstrap_ram;
    }

    if (i2c_ram != NULL) {
        runtime_config_init_defaults(i2c_ram);
    }

    service_tca6408_init();
    service_tca6408_post_bootstrap();
}

const app_context_t *app_bootstrap_context(void)
{
    if (s_bootstrap_ctx == NULL) {
        s_bootstrap_ctx = app_context_get();
    }

    return s_bootstrap_ctx;
}