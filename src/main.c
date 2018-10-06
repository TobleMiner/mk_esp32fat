#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#include "ff.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "diskio.h"
#include "diskio_wl.h"
#include "esp_spi_flash.h"

extern void _spi_flash_init(const char* chip_size, size_t block_size, size_t sector_size, size_t page_size, const char* partition_bin);

extern esp_err_t spi_flash_mmap(size_t src_addr, size_t size, spi_flash_mmap_memory_t memory, const void** out_ptr, spi_flash_mmap_handle_t* out_handle);

extern size_t convert_chip_size_string(const char* chip_size_str);

static esp_err_t __fat_add_path(char* local_prefix, char* dir, char* name);

#define DIRENT_FOR_EACH(cursor, dir) \
    for((cursor) = readdir((dir)); (cursor); (cursor) = readdir((dir)))

static char* pathcat(char* base, char* name) {
	size_t len = strlen(base) + 1 + strlen(name) + 1;
	char* str = calloc(1, len);
	if(!str) {
		return NULL;
	}
	strcat(str, base);
	strcat(str, "/");
	strcat(str, name);
	return str;
}

static void fat_normalize_name(char* name) {
	if(name[0] == '/') {
		memmove(name, name + 1, strlen(name));
	}
}

static esp_err_t fat_add_file(char* local_prefix, char* path) {
	FIL file;
	int fd;
	ssize_t len;
	char buffer[4096];
	esp_err_t err = ESP_OK;
	printf("Adding static file from '%s/%s' as %s\n", local_prefix, path, path);
	char* local_path = pathcat(local_prefix, path);
	if(!local_path) {
		err = -ENOMEM;
		goto fail;
	}

	fat_normalize_name(path);

	printf("Creating file on fatfs: '%s'\n", path);
	if((err = f_open(&file, path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE))) {
		printf("Failed to open file on fatfs: %d\n", err);
		goto fail_local_alloc;
	}

	if((fd = open(local_path, O_RDONLY)) < 0) {
		printf("Failed to open file on local fs\n");
		err = fd;
		goto fail_fat_open;
	}

	while((len = read(fd, buffer, sizeof(buffer))) > 0) {
		ssize_t remainder = len;
		while(remainder > 0) {
			UINT wrlen;
			if((err = f_write(&file, buffer + len - remainder, remainder, &wrlen)) != FR_OK) {
				printf("Failed to write to fat fs\n");
				goto fail_fat_open;			
			}
			remainder -= wrlen;
		}
	}

	if(len < 0) {
		printf("Faled to read from fd\n");
		err = -len;
	} else {
		printf("fd read ok\n");
		err = ESP_OK;
	}

fail_fat_open:
	f_close(&file);
fail_local_alloc:
	free(local_path);
fail:
	return err;
}

#define fat_add_directory(local_prefix, path) \
	__fat_add_directory(local_prefix, path, false)

static esp_err_t __fat_add_directory(char* local_prefix, char* path, bool do_not_create_dirs) {
	esp_err_t err = ESP_OK;
	struct dirent* cursor;
	DIR* dir;
	FILINFO finfo;
	char* local_path = pathcat(local_prefix, path);
	if(!local_path) {
		err = -ENOMEM;
		goto fail;
	}

	printf("Adding directory from '%s' as %s\n", local_path, path);

	fat_normalize_name(path);

	if(!do_not_create_dirs) {
		if(!(err = f_stat(path, &finfo))) {
			printf("Unlinking file '%s'\n", path);
			if((err = f_unlink(path))) {
				goto fail_path_alloc;
			}
		}

		printf("Creating directory '%s'\n", path);
		if((err = f_mkdir(path))) {
			goto fail_path_alloc;
		}
	}

	printf("Opening directory '%s' ...\n", path);
	dir = opendir(local_path);
	if(!dir) {
		err = errno;
		goto fail_path_alloc;
	}
	printf("Opened directory\n");
	DIRENT_FOR_EACH(cursor, dir) {
		printf("Iterating over directory, ent: %p\n", cursor);
		if(!strcmp(cursor->d_name, ".") || !strcmp(cursor->d_name, "..")) {
			printf("Found reference to current/parent directory, skipping\n");
			continue;
		}
		if(!cursor) {
			err = errno;
			break;
		}
		if((err = __fat_add_path(local_prefix, path, cursor->d_name))) {
			goto fail_path_alloc;
		}
	}

fail_path_alloc:
	free(local_path);
fail:
	printf("Closing directory\n");
	closedir(dir);
	return err;
}

static esp_err_t __fat_add_path(char* local_prefix, char* dir, char* name) {
	esp_err_t err;
	struct stat pathinfo;
	char* local_path;
	char* path = name;
	if(dir) {
		path = pathcat(dir, name);
		if(!path) {
			err = ENOMEM;
			goto fail;
		}
	}
	local_path = pathcat(local_prefix, path);
	if(!local_path) {
		err = -ENOMEM;
		goto fail_alloc;
	}

	printf("Stating '%s'\n", local_path);
	if(stat(local_path, &pathinfo)) {
		printf("Stat failed: %s(%d)\n", strerror(errno), errno);
		err = errno;
		goto fail_local_alloc;
	}
	printf("Stat ok\n");
	if(pathinfo.st_mode & S_IFDIR) {
		printf("Stat: dir\n");
		err = fat_add_directory(local_prefix, path);
	} else if(pathinfo.st_mode & S_IFREG) {
		printf("Stat: file\n");
		err = fat_add_file(local_prefix, path);
	} else {
		err = EINVAL;
	}

fail_local_alloc:
	free(local_path);
fail_alloc:
	if(dir) {
		free(path);
	}
fail:
	return err;

}

#define fat_add_path(local_prefix, name) \
	__fat_add_path(local_prefix, NULL, name)

int main(int argc, char** argv) {
	esp_err_t err;
	int fd;
	char* flash_ptr;
	spi_flash_mmap_handle_t hndl;

    _spi_flash_init(CONFIG_ESPTOOLPY_FLASHSIZE, CONFIG_WL_SECTOR_SIZE * 16, CONFIG_WL_SECTOR_SIZE, CONFIG_WL_SECTOR_SIZE, "partition_table.bin");

    FRESULT fr_result;
    BYTE pdrv;
    FATFS fs;
    UINT bw;

    esp_err_t esp_result;

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    
    // Mount wear-levelled partition
    wl_handle_t wl_handle;
    esp_result = wl_mount(partition, &wl_handle);
    assert(esp_result == ESP_OK);
    printf("Mounted wear-leavelled partition\n");

    // Get a physical drive
    esp_result = ff_diskio_get_drive(&pdrv);
    assert(esp_result == ESP_OK);

    // Register physical drive as wear-levelled partition
    esp_result = ff_diskio_register_wl_partition(pdrv, wl_handle);

    // Create FAT volume on the entire disk
    DWORD part_list[] = {100, 0, 0, 0};
    BYTE work_area[FF_MAX_SS];

    fr_result = f_fdisk(pdrv, part_list, work_area);
    assert(fr_result == FR_OK);
    fr_result = f_mkfs("", FM_ANY, 0, work_area, sizeof(work_area)); // Use default volume

    // Mount the volume
    fr_result = f_mount(&fs, "", 0);
    assert(fr_result == FR_OK);

	err = __fat_add_directory("image", "", true);
	printf("Return value: %d\n", err);
	assert(err == ESP_OK);

	spi_flash_mmap(0, 0, 0, &flash_ptr, &hndl);

	flash_ptr += partition->address;

	size_t offset = 0;

	const char* fatfs_image = "fatfs.img";

	printf("Saving to '%s'\n", fatfs_image);
	if((fd = open(fatfs_image, O_RDWR | O_CREAT)) < 0) {
		err = errno;
		printf("Failed to open image file: %s(%d)\n", strerror(err), err);
		goto fail;
	}

	printf("Saving %zu bytes\n", partition->size);

	while(offset < partition->size) {
		ssize_t write_len = write(fd, flash_ptr + offset, partition->size - offset);
		if(write_len < 0) {
			err = errno;
			printf("Failed to write image file: %s(%d)\n", strerror(err), err);
			goto fail;
		}
		offset += write_len;
	} 

	close(fd);

	printf("Image write successful\n");

    // Unmount default volume
    fr_result = f_mount(0, "", 0);
    assert(fr_result == FR_OK);

fail:
    return err;
}
