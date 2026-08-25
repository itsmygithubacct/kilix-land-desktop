#include "provider_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KILIX_PROVIDER_EXIT_USAGE = 2,
    KILIX_PROVIDER_EXIT_INVALID_REQUEST = 3,
    KILIX_PROVIDER_EXIT_UNAVAILABLE = 4,
    KILIX_PROVIDER_EXIT_INTERNAL_ERROR = 70
};

static int emit_description(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"capabilities\":%s,\"config_schema\":"
        "\"kilix.desktop.config.provider.v1\",\"contract_version\":1,"
        "\"display_modes\":%s,\"provider_id\":\"%s\","
        "\"provider_version\":\"%s\",\"required_capabilities\":%s,"
        "\"schema_version\":1}\n",
        provider->capabilities_json, provider->display_modes_json,
        provider->provider_id, provider->provider_version,
        provider->required_capabilities_json) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_check(const struct kilix_provider_v1 *provider)
{
    const char *check_status = provider->check_ready ? "pass" : "unavailable";
    const char *status = provider->check_ready ? "ready" : "unavailable";
    const char *summary = provider->check_ready
                              ? provider->ready_summary
                              : provider->unavailable_summary;
    const char *check_summary = provider->check_ready
                                    ? provider->check_pass_summary
                                    : provider->check_unavailable_summary;
    return printf(
        "{\"checks\":[{\"id\":\"%s\",\"required\":true,"
        "\"status\":\"%s\",\"summary\":\"%s\"}],"
        "\"contract_version\":1,\"provider_id\":\"%s\","
        "\"schema_version\":1,\"status\":\"%s\",\"summary\":\"%s\"}\n",
        provider->check_id, check_status, check_summary,
        provider->provider_id, status, summary) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_config_schema(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"$id\":\"https://schemas.kilix.org/desktop/config/%s/v1\","
        "\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"additionalProperties\":true,\"properties\":{},\"type\":\"object\","
        "\"x-kilix-contract-version\":1,\"x-kilix-provider-id\":\"%s\"}\n",
        provider->provider_id, provider->provider_id) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int emit_config(const struct kilix_provider_v1 *provider)
{
    return printf(
        "{\"contract_version\":1,\"provider_id\":\"%s\",\"revision\":0,"
        "\"schema_version\":1,\"values\":{}}\n",
        provider->provider_id) < 0
               ? KILIX_PROVIDER_EXIT_INTERNAL_ERROR
               : 0;
}

static int ascii_alphanumeric(unsigned char character)
{
    return (character >= (unsigned char)'A' &&
            character <= (unsigned char)'Z') ||
           (character >= (unsigned char)'a' &&
            character <= (unsigned char)'z') ||
           (character >= (unsigned char)'0' &&
            character <= (unsigned char)'9');
}

static int valid_session_id(const char *value)
{
    size_t index;
    size_t length;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 128u || !ascii_alphanumeric((unsigned char)value[0]))
        return 0;
    for (index = 1u; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!ascii_alphanumeric(character) && character != (unsigned char)'.' &&
            character != (unsigned char)'_' && character != (unsigned char)'-')
            return 0;
    }
    return 1;
}

static int valid_version(const char *value)
{
    size_t index;
    size_t length;
    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0u || length > 64u) return 0;
    for (index = 0u; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character < 0x20u || character == 0x7fu) return 0;
    }
    return 1;
}

static int unavailable(const char *provider_id, const char *feature)
{
    (void)fprintf(stderr, "%s: %s is unavailable\n", provider_id, feature);
    return KILIX_PROVIDER_EXIT_UNAVAILABLE;
}

int kilix_provider_v1_dispatch(int argc, char **argv,
                               const struct kilix_provider_v1 *provider)
{
    if (argc < 2 || strcmp(argv[1], "provider") != 0)
        return KILIX_PROVIDER_NOT_HANDLED;
    if (provider == NULL) return KILIX_PROVIDER_EXIT_INTERNAL_ERROR;

    if (argc == 4 && strcmp(argv[2], "describe") == 0 &&
        strcmp(argv[3], "--json") == 0)
        return emit_description(provider);
    if (argc == 4 && strcmp(argv[2], "check") == 0 &&
        strcmp(argv[3], "--json") == 0)
        return emit_check(provider);
    if (argc == 5 && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "schema") == 0 && strcmp(argv[4], "--json") == 0)
        return emit_config_schema(provider);
    if ((argc == 5 || argc == 6) && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "get") == 0 &&
        strcmp(argv[argc - 1], "--json") == 0 &&
        (argc == 5 || argv[4][0] != '\0'))
        return emit_config(provider);
    if (argc == 6 && strcmp(argv[2], "config") == 0 &&
        strcmp(argv[3], "set") == 0)
        return unavailable(provider->provider_id,
                           "protocol configuration writes");

    if (argc >= 3 && strcmp(argv[2], "launch") == 0) {
        if (argc == 3) return KILIX_PROVIDER_LAUNCH;
        if (argc == 5 && strcmp(argv[3], "--session-id") == 0) {
            if (!valid_session_id(argv[4])) {
                (void)fprintf(stderr, "%s: invalid provider session ID\n",
                              provider->provider_id);
                return KILIX_PROVIDER_EXIT_INVALID_REQUEST;
            }
            if (setenv("KILIX_DESKTOP_SESSION_ID", argv[4], 1) != 0)
                return KILIX_PROVIDER_EXIT_INTERNAL_ERROR;
            return KILIX_PROVIDER_LAUNCH;
        }
        (void)fprintf(stderr,
                      "%s: usage: provider launch [--session-id ID]\n",
                      provider->provider_id);
        return KILIX_PROVIDER_EXIT_USAGE;
    }

    if (argc >= 3 && strcmp(argv[2], "screenshot") == 0) {
        if (argc < 4 || argv[3][0] == '\0') {
            (void)fprintf(stderr,
                          "%s: usage: provider screenshot OUTPUT [OPTIONS...]\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_USAGE;
        }
        if (!provider->screenshot_available)
            return unavailable(provider->provider_id, "headless screenshot");
        return KILIX_PROVIDER_SCREENSHOT;
    }

    if (argc >= 3 && strcmp(argv[2], "migrate") == 0) {
        if ((argc != 5 && argc != 6) || strcmp(argv[3], "--from") != 0 ||
            (argc == 6 && strcmp(argv[5], "--dry-run") != 0)) {
            (void)fprintf(stderr,
                          "%s: usage: provider migrate --from VERSION [--dry-run]\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_USAGE;
        }
        if (!valid_version(argv[4])) {
            (void)fprintf(stderr, "%s: invalid migration source version\n",
                          provider->provider_id);
            return KILIX_PROVIDER_EXIT_INVALID_REQUEST;
        }
        return unavailable(provider->provider_id,
                           "protocol persistence migration");
    }

    (void)fprintf(stderr, "%s: unknown provider protocol command\n",
                  provider->provider_id);
    return KILIX_PROVIDER_EXIT_USAGE;
}
