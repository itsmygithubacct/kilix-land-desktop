#ifndef KILIX_LAND_DESKTOP_PROVIDER_PROTOCOL_H
#define KILIX_LAND_DESKTOP_PROVIDER_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

enum kilix_provider_dispatch_result {
    KILIX_PROVIDER_SCREENSHOT = -3,
    KILIX_PROVIDER_LAUNCH = -2,
    KILIX_PROVIDER_NOT_HANDLED = -1
};

struct kilix_provider_v1 {
    const char *provider_id;
    const char *provider_version;
    const char *display_modes_json;
    const char *capabilities_json;
    const char *required_capabilities_json;
    const char *check_id;
    const char *ready_summary;
    const char *unavailable_summary;
    const char *check_pass_summary;
    const char *check_unavailable_summary;
    bool check_ready;
    bool screenshot_available;
};

int kilix_provider_v1_dispatch(int argc, char **argv,
                               const struct kilix_provider_v1 *provider);
bool kilix_provider_v1_storage_available(void);
bool kilix_provider_v1_storage_required(void);
bool kilix_provider_v1_storage_path(const char *provider_id,
                                    const char *category,
                                    char *output, size_t output_size);
bool kilix_provider_v1_storage_value(const char *provider_id,
                                     const char *key,
                                     char *output, size_t output_size);
bool kilix_provider_v1_storage_set(const char *provider_id,
                                   const char *key, const char *value);

#endif
