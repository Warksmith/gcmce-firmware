#pragma once

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2


void sd_init(bool reinit);
void sd_unmount(void);
int sd_open(const char *path, int oflag);
int sd_close(int fd);
void sd_flush(int fd);
int sd_read(int fd, void *buf, size_t count);
int sd_write(int fd, void *buf, size_t count);
int sd_seek(int fd, int32_t offset, int whence);
uint32_t sd_tell(int fd);

int sd_filesize(int fd);
int sd_mkdir(const char *path);
int sd_exists(const char *path);

int sd_remove(const char* path);

int sd_iterate_dir(int dir, int it);
size_t sd_get_name(int fd, char* name, size_t size);
bool sd_is_dir(int fd);
int sd_fd_is_open(int fd);


/* raw sector access, never used together with the functions above */
uint32_t sd_raw_init(void); /* returns the sector count, 0 if there is no usable card */
bool sd_raw_read(uint32_t sector, uint8_t *dst, size_t count);
bool sd_raw_write(uint32_t sector, const uint8_t *src, size_t count);
bool sd_raw_sync(void);

bool sd_read_sector(uint32_t sector, uint8_t* dst);
bool sd_write_sector(uint32_t sector, const uint8_t* src);

/**
 * Force a sync of the SD card cache to ensure all pending writes are committed
 * Note: This must only be called from Core 0
 *
 * @return true if sync successful, false if error
 */
bool sd_sync_cache(void);
