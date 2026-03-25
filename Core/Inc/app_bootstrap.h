#ifndef INC_APP_BOOTSTRAP_H_
#define INC_APP_BOOTSTRAP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "service_runtime_config.h"

typedef struct {
    uint8_t *i2c_ram;
    app_context_t *context;
} app_bootstrap_init_t;

void app_bootstrap_pre_init(const app_bootstrap_init_t *init);
void app_bootstrap_wire(void);
void app_bootstrap_start(void);
const app_context_t *app_bootstrap_context(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_BOOTSTRAP_H_ */
