#pragma once
#include <stdint.h>
typedef enum {
    ODROID_START_ACTION_NORMAL = 0,
    ODROID_START_ACTION_RESTART,
    ODROID_START_ACTION_PAX
} ODROID_START_ACTION;
#ifdef __cplusplus
extern "C" {
#endif
char *odroid_util_GetFileName(const char *path);
char *odroid_util_GetFileExtenstion(const char *path);
char *odroid_util_GetFileNameWithoutExtension(const char *path);
void odroid_settings_RomFilePath_set(const char *path);
int32_t odroid_settings_Volume_get(void);
void odroid_settings_Volume_set(int32_t value);
ODROID_START_ACTION odroid_settings_StartAction_get(void);
void odroid_settings_StartAction_set(ODROID_START_ACTION value);
#ifdef __cplusplus
}
#endif
