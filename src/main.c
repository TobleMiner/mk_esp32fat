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

#define MAX_PATH_LEN 256
#define DEFAULT_IMAGE_DIR "image"

extern void _spi_flash_init(const char* chip_size, size_t block_size, size_t sector_size, size_t page_size, const char* partition_bin);

extern esp_err_t spi_flash_mmap(size_t src_addr, size_t size, spi_flash_mmap_memory_t memory, const void** out_ptr, spi_flash_mmap_handle_t* out_handle);

extern size_t convert_chip_size_string(const char* chip_size_str);

static esp_err_t fat_add_path(char* local_path, char* fat_path);

#define DIRENT_FOR_EACH(cursor, dir) \
	for((cursor) = readdir((dir)); (cursor); (cursor) = readdir((dir)))

static void pathcat(char* path, size_t path_len, char* base, char* name) {
	memset(path, 0, path_len);
	snprintf(path, path_len, "%s/%s", base, name);
}

static esp_err_t fat_add_file(char* local_path, char* fat_path) {
	FIL file;
	int fd;
	ssize_t len;
	char buffer[4096];
	esp_err_t err = ESP_OK;

	printf("\tAdding file '%s' => '%s'\n", local_path, fat_path);

	if((err = f_open(&file, fat_path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE))) {
		printf("Failed to open file '%s' on fatfs: %d\n", fat_path, err);
		goto fail;
	}

	if((fd = open(local_path, O_RDONLY)) < 0) {
		fprintf(stderr, "Failed to open file on local fs\n");
		err = fd;
		goto fail_fat_open;
	}

	// Read data chunkwise and write it to the wear leveling fatfs
	while((len = read(fd, buffer, sizeof(buffer))) > 0) {
		ssize_t remainder = len;
		while(remainder > 0) {
			UINT wrlen;
			if((err = f_write(&file, buffer + len - remainder, remainder, &wrlen))) {
				fprintf(stderr, "Failed to write to fat fs: %d\n", err);
				goto fail_local_open;
			}
			remainder -= wrlen;
		}
	}

	err = ESP_OK;
	if(len < 0) {
		err = errno;
		fprintf(stderr, "Faled to read from local file: %s(%d)\n", strerror(err), err);
	}

fail_local_open:
	close(fd);
fail_fat_open:
	f_close(&file);
fail:
	return err;
}

static esp_err_t fat_add_directory_contents(char* local_path, char* fat_path) {
	esp_err_t err = ESP_OK;
	struct dirent* cursor;
	DIR* dir = opendir(local_path);
	if(!dir) {
		err = errno;
		goto fail;
	}

	// readdir does not touch errno on success, thus we should set it to zero
	errno = 0;
	DIRENT_FOR_EACH(cursor, dir) {
		/* Putting this on stack might be a problem when using large,
		 * deeply nested fs images, maybe we should move it to heap?
		 */
		char local_entry_path[MAX_PATH_LEN];
		char fat_entry_path[MAX_PATH_LEN];

		if(!cursor) {
			err = errno;
			break;
		}

		// Ingore current and parent directory
		if(!strcmp(cursor->d_name, ".") || !strcmp(cursor->d_name, "..")) {
			continue;
		}

		pathcat(local_entry_path, sizeof(local_entry_path), local_path, cursor->d_name);
		pathcat(fat_entry_path, sizeof(fat_entry_path), fat_path, cursor->d_name);
		if((err = fat_add_path(local_entry_path, fat_entry_path))) {
			goto fail;
		}
	}

fail:
	closedir(dir);
	return err;
}

static esp_err_t fat_add_directory(char* local_path, char* fat_path) {
	esp_err_t err = ESP_OK;
	FILINFO finfo;

	if(!(err = f_stat(fat_path, &finfo))) {
		if((err = f_unlink(fat_path))) {
			fprintf(stderr, "Failed to unlink '%s'\n", fat_path);
			goto fail;
		}
	}

	printf("\tAdding directory '%s' => '%s'\n", local_path, fat_path);
	if((err = f_mkdir(fat_path))) {
		goto fail;
	}

	err = fat_add_directory_contents(local_path, fat_path);

fail:
	return err;
}

static esp_err_t fat_add_path(char* local_path, char* fat_path) {
	esp_err_t err;
	struct stat pathinfo;

	if(stat(local_path, &pathinfo)) {
		printf("Stat failed: %s(%d)\n", strerror(errno), errno);
		err = errno;
		goto fail;
	}

	if(pathinfo.st_mode & S_IFDIR) {
		err = fat_add_directory(local_path, fat_path);
	} else if(pathinfo.st_mode & S_IFREG) {
		err = fat_add_file(local_path, fat_path);
	} else {
		err = EINVAL;
	}

fail:
	return err;

}

void show_usage(char* prgrm) {
	fprintf(stderr, "Usage: %s [-c <fatfs directory>] <fatfs image name>\n", prgrm);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t -c <fatfs directory>\tSet directory to build fatfs from to <fatfs directory>. Defaults to '%s'\n", DEFAULT_IMAGE_DIR);
};

int main(int argc, char** argv) {
	esp_err_t err;
	int fd, opt;
	char* flash_ptr;
	size_t offset = 0;
	spi_flash_mmap_handle_t hndl;
	wl_handle_t wl_handle;
	FRESULT fr_result;
	BYTE pdrv;
	FATFS fs;
	UINT bw;
	DWORD part_list[] = {100, 0, 0, 0};
	BYTE work_area[FF_MAX_SS];
	const esp_partition_t* partition;

	char* image_src_dir = DEFAULT_IMAGE_DIR;
	const char* fatfs_image;

	while((opt = getopt(argc, argv, "c:h")) >= 0) {
		switch(opt) {
			case 'c':
				image_src_dir = strdup(optarg);
				if(!image_src_dir) {
					err = ENOMEM;
					fprintf(stderr, "Failed to allocate memory for image_src_dir\n");
					goto fail;
				}
				break;
			case 'h':
			default:
				show_usage(argv[0]);
				err = -1;
				goto fail;
		}
	}

	if(optind >= argc) {
		fprintf(stderr, "Missing required positional argument <fatfs image name>\n");
		show_usage(argv[0]);
		err = -1;
		goto fail;
	}

	fatfs_image = argv[optind];

	_spi_flash_init(CONFIG_ESPTOOLPY_FLASHSIZE, CONFIG_WL_SECTOR_SIZE * 16, CONFIG_WL_SECTOR_SIZE, CONFIG_WL_SECTOR_SIZE, "partition_table.bin");

	partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");

	// Mount wear-levelled partition
	if((err = wl_mount(partition, &wl_handle))) {
		fprintf(stderr, "Failed to mount partition: %d\n", err);
		goto fail;
	}


	// Get emulated physical drive
	if((err = ff_diskio_get_drive(&pdrv))) {
		fprintf(stderr, "Failed to get emulated drive: %d\n", err);
		goto fail_mount;
	}

	// Get wear leveling partition wrapper
	if((err = ff_diskio_register_wl_partition(pdrv, wl_handle))) {
		fprintf(stderr, "Failed to get wear leveling wrapper for patition: %d\n", err);
		goto fail_mount;
	}

	// Create fatfs partition table
	if((fr_result = f_fdisk(pdrv, part_list, work_area))) {
		err = fr_result;
		fprintf(stderr, "Failed to create fatfs partition table: %d\n", err);
		goto fail_mount;
	}

	// Create fatfs fat filesystem
	if((fr_result = f_mkfs("", FM_ANY, 0, work_area, sizeof(work_area)))) {
		err = fr_result;
		fprintf(stderr, "Failed to create fatfs filesystem: %d\n", err);
		goto fail_mount;
	}

	if((fr_result = f_mount(&fs, "", 0))) {
		err = fr_result;
		fprintf(stderr, "Failed to mount fatfs filesystem: %d\n", err);
		goto fail_mount;
	}

	printf("Adding files:\n");
	if((err = fat_add_directory_contents(image_src_dir, ""))) {
		fprintf(stderr, "Failed to add files to fat image: %s(%d)\n", strerror(err), err);
		goto fail_mount;

	}

	// Use mmap stub wrapper to obtain pointer to flash memory buffer
	spi_flash_mmap(0, 0, 0, (const void**)&flash_ptr, &hndl);

	// Move pointer to start of data partition
	flash_ptr += partition->address;

	printf("Saving fatfs image to '%s'\n", fatfs_image);
	if((fd = open(fatfs_image, O_RDWR | O_CREAT, 0644)) < 0) {
		err = errno;
		fprintf(stderr, "Failed to open image file: %s(%d)\n", strerror(err), err);
		goto fail_mount;
	}

	printf("Saving %zu bytes to file\n", partition->size);

	while(offset < partition->size) {
		ssize_t write_len = write(fd, flash_ptr + offset, partition->size - offset);
		if(write_len < 0) {
			err = errno;
			fprintf(stderr, "Failed to write image file: %s(%d)\n", strerror(err), err);
			goto fail_image_open;
		}
		offset += write_len;
	}

	printf("Image complete\n");

fail_image_open:
	close(fd);
fail_mount:
	f_mount(0, "", 0);
fail:
	return err;
}
