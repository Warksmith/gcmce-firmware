#include "card_config.h"
#include "sd.h"
#include "debug.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ini.h"


#if LOG_LEVEL_CARD_CONF == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_CARD_CONF, level, fmt, ##x)
#endif

#define MAX_CFG_PATH_LENGTH         (96)
#define CUSTOM_CARDS_CONFIG_PATH    (".flippermce/Game2Folder.ini")

typedef struct {
    const char *channel_number;
    char *channel_name;
    size_t channel_name_max_len;
    uint8_t card_size;
    uint8_t max_channels;
    card_backend_t backend;
} parse_card_config_t;

typedef struct {
    const char *game_id;
    char *card_folder;
    size_t card_folder_max_len;
    size_t best_match;
    bool matched;
    bool use_default;
    char default_card[MAX_FOLDER_NAME_LENGTH];
} parse_custom_card_folder_t;

static int parse_custom_card_folder(void *user, const char *section, const char *name, const char *value) {
    parse_custom_card_folder_t *ctx = user;

    if (strcmp(section, "GC") != 0)
        return 1;

    if (strcmp(name, "UseDefaultCard") == 0) {
        ctx->use_default = strcasecmp(value, "ON") == 0;
        return 1;
    }
    if (strcmp(name, "DefaultCard") == 0) {
        strlcpy(ctx->default_card, value, sizeof(ctx->default_card));
        return 1;
    }

    /* Only trailing stars: a bounded prefix comparison, without backtracking.
     * Exact IDs outrank all prefixes; equal specificity keeps last-entry wins. */
    size_t rank;
    const char *star = strchr(name, '*');
    if (star != NULL) {
        rank = (size_t)(star - name);
        const char *tail = star;
        while (*tail == '*')
            tail++;
        if (*tail != '\0' || strncmp(name, ctx->game_id, rank) != 0)
            return 1;
    } else {
        if (strcmp(name, ctx->game_id) != 0)
            return 1;
        rank = SIZE_MAX;
    }
    if (!ctx->matched || rank >= ctx->best_match) {
        ctx->matched = true;
        ctx->best_match = rank;
        strlcpy(ctx->card_folder, value, ctx->card_folder_max_len);
    }

    log(LOG_TRACE, "s=%s n=%s card_folder=%s\n", section, name, ctx->card_folder);

    return 1;
}

static int parse_card_configuration(void *user, const char *section, const char *name, const char *value) {
    parse_card_config_t *ctx = user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("ChannelName", ctx->channel_number)) {
        if (strlen(value) <= ctx->channel_name_max_len) {
            strcpy(ctx->channel_name, value);
        }
    } else if (MATCH("Settings", "CardSize")) {
        uint8_t size = (uint8_t)atoi(value);
        switch (size) {
            case 4:
            case 8:
            case 16:
            case 32:
            case 64:
                ctx->card_size = size;
                break;
            default:
                break;
        }
    } else if (MATCH("Settings", "StorageBackend")) {
        /* anything but an exact FOLDER stays a .raw card */
        ctx->backend = strcmp(value, "FOLDER") == 0 ? CARD_BACKEND_FOLDER : CARD_BACKEND_RAW;
        if (ctx->backend == CARD_BACKEND_RAW && strcmp(value, "RAW") != 0)
            log(LOG_WARN, "invalid StorageBackend %s\n", value);
    } else if (MATCH("Settings", "MaxChannels")) {
        uint8_t max_channels = (uint8_t)atoi(value);
        if (max_channels > 0) {
            ctx->max_channels = max_channels;
        }
    }
    #undef MATCH

    return 1;
}

static void card_config_get_ini_name(const char* card_folder, const char* card_base, char* config_path) {
    snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/GC/%s/%s.ini", card_folder, card_base);
    log(LOG_TRACE, "config_path=%s\n", config_path);

}

void card_config_read_channel_name(const char* card_folder, const char* card_base, const char* channel_number, char* name, size_t name_max_len) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        parse_card_config_t ctx = {
            .channel_number = channel_number,
            .channel_name = name,
            .channel_name_max_len = name_max_len,
            .card_size = 0,
            .max_channels = 8
        };
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
}

card_backend_t card_config_get_gc_backend(const char* card_folder, const char* card_base, card_backend_t default_backend) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8,
        .backend = default_backend
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    return ctx.backend;
}

uint8_t card_config_get_gc_cardsize(const char* card_folder, const char* card_base) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    return ctx.card_size;
}

uint8_t card_config_get_max_channels(const char* card_folder, const char* card_base) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        log(LOG_TRACE, "config_path=%s\n", config_path);
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    log(LOG_TRACE, "max_channels=%d\n", ctx.max_channels);
    return ctx.max_channels;
}

bool card_config_read_image(uint8_t buff[1032], const char* card_folder, const char* card_base, int chan_idx) {
    char image_path[64];
    int fd;

    snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/GC/%s/%s-%i.bin", card_folder, card_base, chan_idx);
    if(!sd_exists(image_path)) {
        snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/GC/%s/%s.bin", card_folder, card_base);
    }

    fd = sd_open(image_path, O_RDONLY);
    if (fd >= 0) {
        sd_read(fd, buff, 1032);
        sd_close(fd);
        log(LOG_TRACE, "Read image %s\n", image_path);
        return true;
    } else {
        memset(buff, 0, 1032);
        return false;
    }
}


void card_config_get_card_folder(const char* game_id, char* card_folder, size_t card_folder_max_len) {
    if (card_folder == NULL || card_folder_max_len == 0)
        return;
    card_folder[0] = '\0';
    if (game_id == NULL || game_id[0] == '\0')
        return;
    parse_custom_card_folder_t ctx = {
        .game_id = game_id,
        .card_folder = card_folder,
        .card_folder_max_len = card_folder_max_len
    };

    int fd = sd_open(CUSTOM_CARDS_CONFIG_PATH, O_RDONLY);
    log(LOG_TRACE, "Looking for game_id=%s \n", game_id);

    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_custom_card_folder, &ctx);
        sd_close(fd);
        if (!ctx.matched && ctx.use_default && ctx.default_card[0] != '\0')
            strlcpy(card_folder, ctx.default_card, card_folder_max_len);
    }

    log(LOG_TRACE, "found card_folder=%s\n", card_folder);
}
