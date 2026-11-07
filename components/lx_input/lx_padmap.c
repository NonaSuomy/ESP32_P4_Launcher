/*
 * Controller button mappings as text on the SD card, one line per pad model:
 *
 *   054C:0268  A=B B=A X=Y Y=X  # Sony PLAYSTATION(R)3 Controller
 *
 * VID:PID, then <button the pad reports>=<button it should be> for every button
 * that is not itself ("-" = ignored), then the pad's name. Button names are the
 * PAPP_BTN_ ones: A B X Y L1 R1 L2 R2 SELECT START L3 R3 HOME MISC UP DOWN LEFT RIGHT.
 *
 * /sd/config/gamepads_mine.txt  written by Settings -> Map controller; wins
 * /sd/config/gamepads.txt       the shared database (SDcard/config/gamepads.txt
 *                               in the repository, updated by the Store)
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lx_padmap";

#define MINE_FILE  LX_PADMAP_DIR "/gamepads_mine.txt"
#define DB_FILE    LX_PADMAP_DIR "/gamepads.txt"
#define LINE_MAX_  256
#define FILE_MAX   (64 * 1024)

static const char *const k_names[LX_INPUT_MAP_BITS] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START",
    "L3", "R3", "HOME", "MISC", "BIT14", "BIT15", "UP", "DOWN", "LEFT", "RIGHT",
};

static int name_bit(const char *s, size_t n)
{
    for (int i = 0; i < LX_INPUT_MAP_BITS; i++)
        if (strlen(k_names[i]) == n && strncasecmp(k_names[i], s, n) == 0) return i;
    return -1;
}

/* Line for vid:pid? Then fills map (identity, then the line's changes). */
static bool parse_line(const char *line, uint16_t vid, uint16_t pid, uint8_t *map)
{
    while (*line == ' ' || *line == '\t') line++;
    unsigned v, p;
    int used = 0;
    if (sscanf(line, "%4x:%4x%n", &v, &p, &used) != 2 || v != vid || p != pid) return false;
    for (int i = 0; i < LX_INPUT_MAP_BITS; i++) map[i] = (uint8_t)i;
    const char *s = line + used;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') break;
        const char *eq = s;
        while (*eq && *eq != '=' && !isspace((unsigned char)*eq)) eq++;
        const char *val = eq + 1, *end = val;
        while (*end && !isspace((unsigned char)*end) && *end != '#') end++;
        if (*eq == '=') {
            int from = name_bit(s, eq - s);
            int to = (end - val == 1 && *val == '-') ? 0xFF : name_bit(val, end - val);
            if (from >= 0 && to != -1) map[from] = (uint8_t)to;
            else ESP_LOGW(TAG, "%04X:%04X: '%.*s' not understood", vid, pid, (int)(end - s), s);
        }
        s = end;
    }
    return true;
}

static bool load_from(const char *path, uint16_t vid, uint16_t pid, uint8_t *map)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[LINE_MAX_];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) found = parse_line(line, vid, pid, map);
    fclose(f);
    return found;
}

int lx_padmap_load(uint16_t vid, uint16_t pid, uint8_t *map)
{
    if (load_from(MINE_FILE, vid, pid, map)) return LX_PADMAP_MINE;
    if (load_from(DB_FILE, vid, pid, map)) return LX_PADMAP_DB;
    return LX_PADMAP_NONE;
}

esp_err_t lx_padmap_save(uint16_t vid, uint16_t pid, const char *name, const uint8_t *map)
{
    char *old = heap_caps_calloc(1, FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!old) return ESP_ERR_NO_MEM;
    size_t n = 0;
    FILE *f = fopen(MINE_FILE, "r");
    if (f) {
        n = fread(old, 1, FILE_MAX - 1, f);
        fclose(f);
    }
    mkdir(LX_PADMAP_DIR, 0775);
    f = fopen(MINE_FILE ".tmp", "w");
    if (!f) {
        free(old);
        return ESP_FAIL;
    }
    if (!n) fputs("# Your controller mappings (Settings -> Map controller). To share one, add its line to\n"
                  "# SDcard/config/gamepads.txt in https://github.com/giltal/ESP32_P4_Launcher\n", f);
    uint8_t scratch[LX_INPUT_MAP_BITS];
    for (char *line = old; line < old + n;) {                /* keep every other pad's line */
        char *next = strchr(line, '\n');
        next = next ? next + 1 : old + n;
        if (!parse_line(line, vid, pid, scratch)) fwrite(line, 1, next - line, f);
        line = next;
    }
    if (map) {
        fprintf(f, "%04X:%04X ", vid, pid);
        for (int i = 0; i < LX_INPUT_MAP_BITS; i++)
            if (map[i] != i) fprintf(f, " %s=%s", k_names[i], map[i] < LX_INPUT_MAP_BITS ? k_names[map[i]] : "-");
        fprintf(f, "  # %s\n", name && name[0] ? name : "unnamed controller");
    }
    free(old);
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(MINE_FILE);                  /* FAT rename doesn't replace */
        ok = rename(MINE_FILE ".tmp", MINE_FILE) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "could not write %s", MINE_FILE);
    return ok ? ESP_OK : ESP_FAIL;
}
