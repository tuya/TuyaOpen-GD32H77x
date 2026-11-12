/**
 * @file tkl_fs.c
 * @brief GD32H77X FatFS-backed TKL file system implementation.
 *
 * @copyright Copyright (c) 2026 Tuya Inc. All Rights Reserved.
 */

#include "tkl_fs.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "tal_api.h"
#include "tkl_memory.h"

#define TKL_FS_MOUNT_MAX_LEN 32U

typedef struct {
    DIR     dir;
    FILINFO info;
} TKL_DIR_CTX_T;

static FATFS s_fatfs;
static char s_mount_path[TKL_FS_MOUNT_MAX_LEN];
static bool s_mounted;

static const char *__fs_path(const char *path)
{
    size_t mount_len;

    if (path == NULL || !s_mounted) {
        return NULL;
    }
    mount_len = strlen(s_mount_path);
    if (strncmp(path, s_mount_path, mount_len) != 0) {
        return path;
    }
    path += mount_len;
    while (*path == '/') {
        ++path;
    }
    return path;
}

static void *__file_alloc(void)
{
    TUYA_FILE file = tkl_system_malloc(sizeof(FIL));
    return file;
}

static int __file_close(TUYA_FILE file)
{
    int rt;

    if (file == NULL) {
        return -1;
    }
    rt = f_close((FIL *)file) == FR_OK ? 0 : -1;
    tkl_system_free(file);
    return rt;
}

int32_t tkl_fs_mount(const char *path, FS_DEV_TYPE_T dev_type)
{
    FRESULT fr;

    if (path == NULL || strlen(path) >= sizeof(s_mount_path) ||
        dev_type != DEV_SDCARD) {
        return -1;
    }
    strcpy(s_mount_path, path);
    fr = f_mount(&s_fatfs, "", 1);
    if (fr != FR_OK) {
        s_mounted = false;
        return -1;
    }
    s_mounted = true;
    return 0;
}

int32_t tkl_fs_unmount(const char *path)
{
    (void)path;
    (void)f_mount(NULL, "", 0);
    s_mounted = false;
    return 0;
}

int32_t tkl_fs_mkdir(const char *path)
{
    const char *fs_path = __fs_path(path);
    return (fs_path != NULL && f_mkdir(fs_path) == FR_OK) ? 0 : -1;
}

int32_t tkl_fs_remove(const char *path)
{
    const char *fs_path = __fs_path(path);
    FILINFO info;
    FRESULT fr;

    if (fs_path == NULL || f_stat(fs_path, &info) != FR_OK) {
        return -1;
    }
    fr = (info.fattrib & AM_DIR) ? f_rmdir(fs_path) : f_unlink(fs_path);
    return fr == FR_OK ? 0 : -1;
}

int32_t tkl_fs_mode(const char *path, uint32_t *mode)
{
    const char *fs_path = __fs_path(path);
    FILINFO info;

    if (fs_path == NULL || mode == NULL || f_stat(fs_path, &info) != FR_OK) {
        return -1;
    }
    *mode = (info.fattrib & AM_DIR) ? 0040755U : 0100644U;
    return 0;
}

int32_t tkl_fs_is_exist(const char *path, BOOL_T *is_exist)
{
    const char *fs_path = __fs_path(path);

    if (fs_path == NULL || is_exist == NULL) {
        return -1;
    }
    *is_exist = f_stat(fs_path, NULL) == FR_OK ? TRUE : FALSE;
    return 0;
}

int32_t tkl_fs_rename(const char *path_old, const char *path_new)
{
    const char *old_path = __fs_path(path_old);
    const char *new_path = __fs_path(path_new);

    if (old_path == NULL || new_path == NULL) {
        return -1;
    }
    return f_rename(old_path, new_path) == FR_OK ? 0 : -1;
}

int32_t tkl_dir_open(const char *path, TUYA_DIR *dir)
{
    const char *fs_path = __fs_path(path);
    TKL_DIR_CTX_T *ctx;

    if (fs_path == NULL || dir == NULL) {
        return -1;
    }
    ctx = tkl_system_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return -1;
    }
    if (f_opendir(&ctx->dir, fs_path) != FR_OK) {
        tkl_system_free(ctx);
        return -1;
    }
    *dir = ctx;
    return 0;
}

int32_t tkl_dir_close(TUYA_DIR dir)
{
    TKL_DIR_CTX_T *ctx = (TKL_DIR_CTX_T *)dir;

    if (ctx == NULL) {
        return -1;
    }
    (void)f_closedir(&ctx->dir);
    tkl_system_free(ctx);
    return 0;
}

int32_t tkl_dir_read(TUYA_DIR dir, TUYA_FILEINFO *info)
{
    TKL_DIR_CTX_T *ctx = (TKL_DIR_CTX_T *)dir;

    if (ctx == NULL || info == NULL || f_readdir(&ctx->dir, &ctx->info) != FR_OK ||
        ctx->info.fname[0] == '\0') {
        return -1;
    }
    *info = &ctx->info;
    return 0;
}

int32_t tkl_dir_name(TUYA_FILEINFO info, const char **name)
{
    FILINFO *fs_info = (FILINFO *)info;

    if (fs_info == NULL || name == NULL) {
        return -1;
    }
    *name = fs_info->fname;
    return 0;
}

int32_t tkl_dir_is_directory(TUYA_FILEINFO info, BOOL_T *is_dir)
{
    FILINFO *fs_info = (FILINFO *)info;

    if (fs_info == NULL || is_dir == NULL) {
        return -1;
    }
    *is_dir = (fs_info->fattrib & AM_DIR) ? TRUE : FALSE;
    return 0;
}

int32_t tkl_dir_is_regular(TUYA_FILEINFO info, BOOL_T *is_regular)
{
    FILINFO *fs_info = (FILINFO *)info;

    if (fs_info == NULL || is_regular == NULL) {
        return -1;
    }
    *is_regular = (fs_info->fattrib & AM_DIR) ? FALSE : TRUE;
    return 0;
}

TUYA_FILE tkl_fopen(const char *path, const char *mode)
{
    const char *fs_path = __fs_path(path);
    BYTE fs_mode;
    FIL *file;

    if (fs_path == NULL || mode == NULL) {
        return NULL;
    }
    if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
        fs_mode = FA_READ;
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
        fs_mode = FA_WRITE | FA_CREATE_ALWAYS;
    } else if (strcmp(mode, "a") == 0 || strcmp(mode, "ab") == 0) {
        fs_mode = FA_WRITE | FA_OPEN_APPEND;
    } else if (strcmp(mode, "r+") == 0 || strcmp(mode, "rb+") == 0) {
        fs_mode = FA_READ | FA_WRITE;
    } else if (strcmp(mode, "w+") == 0 || strcmp(mode, "wb+") == 0) {
        fs_mode = FA_READ | FA_WRITE | FA_CREATE_ALWAYS;
    } else if (strcmp(mode, "a+") == 0 || strcmp(mode, "ab+") == 0) {
        fs_mode = FA_READ | FA_WRITE | FA_OPEN_APPEND;
    } else {
        return NULL;
    }

    file = __file_alloc();
    if (file == NULL) {
        return NULL;
    }
    if (f_open(file, fs_path, fs_mode) != FR_OK) {
        tkl_system_free(file);
        return NULL;
    }
    return file;
}

int32_t tkl_fclose(TUYA_FILE file)
{
    return __file_close(file);
}

int32_t tkl_fread(void *buf, int32_t bytes, TUYA_FILE file)
{
    UINT read_len = 0U;

    if (buf == NULL || bytes < 0 || file == NULL ||
        f_read((FIL *)file, buf, (UINT)bytes, &read_len) != FR_OK) {
        return -1;
    }
    return (int)read_len;
}

int32_t tkl_fwrite(void *buf, int32_t bytes, TUYA_FILE file)
{
    UINT write_len = 0U;

    if (buf == NULL || bytes < 0 || file == NULL ||
        f_write((FIL *)file, buf, (UINT)bytes, &write_len) != FR_OK) {
        return -1;
    }
    return (int)write_len;
}

int32_t tkl_fsync(int32_t fd)
{
    TUYA_FILE file = (TUYA_FILE)(uintptr_t)(uint32_t)fd;
    return (file != NULL && f_sync((FIL *)file) == FR_OK) ? 0 : -1;
}

char *tkl_fgets(char *buf, int32_t len, TUYA_FILE file)
{
    if (buf == NULL || len <= 0 || file == NULL) {
        return NULL;
    }
    return f_gets(buf, len, (FIL *)file);
}

int32_t tkl_feof(TUYA_FILE file)
{
    return (file != NULL && f_eof((FIL *)file)) ? 1 : 0;
}

int32_t tkl_fseek(TUYA_FILE file, int64_t offs, int32_t whence)
{
    FIL *fp = (FIL *)file;
    FSIZE_t base;

    if (fp == NULL) {
        return -1;
    }
    if (whence == SEEK_SET) {
        base = 0U;
    } else if (whence == SEEK_CUR) {
        base = f_tell(fp);
    } else if (whence == SEEK_END) {
        base = f_size(fp);
    } else {
        return -1;
    }
    if (offs < 0 && (uint64_t)(-offs) > base) {
        return -1;
    }
    return f_lseek(fp, (FSIZE_t)((int64_t)base + offs)) == FR_OK ? 0 : -1;
}

int64_t tkl_ftell(TUYA_FILE file)
{
    return file == NULL ? -1 : (int64_t)f_tell((FIL *)file);
}

int32_t tkl_fgetsize(const char *filepath)
{
    const char *fs_path = __fs_path(filepath);
    FILINFO info;

    if (fs_path == NULL || f_stat(fs_path, &info) != FR_OK) {
        return -1;
    }
    return (int)info.fsize;
}

int32_t tkl_faccess(const char *filepath, int32_t mode)
{
    const char *fs_path = __fs_path(filepath);
    FIL file;

    (void)mode;
    if (fs_path == NULL || f_open(&file, fs_path, FA_READ) != FR_OK) {
        return -1;
    }
    (void)f_close(&file);
    return 0;
}

int32_t tkl_fgetc(TUYA_FILE file)
{
    uint8_t ch;
    UINT read_len = 0U;

    if (file == NULL ||
        f_read((FIL *)file, &ch, 1U, &read_len) != FR_OK ||
        read_len != 1U) {
        return EOF;
    }
    return ch;
}

int32_t tkl_fflush(TUYA_FILE file)
{
    return (file != NULL && f_sync((FIL *)file) == FR_OK) ? 0 : -1;
}

int32_t tkl_fileno(TUYA_FILE file)
{
    return (int)(uintptr_t)file;
}

int32_t tkl_ftruncate(int32_t fd, uint64_t length)
{
    TUYA_FILE file = (TUYA_FILE)(uintptr_t)(uint32_t)fd;

    if (file == NULL || length > (uint64_t)(~(FSIZE_t)0)) {
        return -1;
    }
    if (f_lseek((FIL *)file, (FSIZE_t)length) != FR_OK) {
        return -1;
    }
    return f_truncate((FIL *)file) == FR_OK ? 0 : -1;
}
