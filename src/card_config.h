#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_FOLDER_NAME_LENGTH      ( 32 )

typedef enum {
    CARD_BACKEND_RAW = 0,
    CARD_BACKEND_FOLDER
} card_backend_t;

void card_config_read_channel_name(const char* card_folder, const char* card_base, const char* channel_number, char* name, size_t name_max_len);
uint8_t card_config_get_max_channels(const char* card_folder, const char* card_base);
uint8_t card_config_get_gc_cardsize(const char* card_folder, const char* card_base);
card_backend_t card_config_get_gc_backend(const char* card_folder, const char* card_base, card_backend_t default_backend);
/* Exact ID, then longest trailing-star prefix, then opt-in DefaultCard.
 * Equal specificity uses the last entry. Returns an empty string if unmapped.
 * card_folder_max_len includes the terminating NUL. */
void card_config_get_card_folder(const char* game_id, char* card_folder, size_t card_folder_max_len);
bool card_config_read_image(uint8_t buff[1032], const char* card_folder, const char* card_base, int chan_idx);
