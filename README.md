ESP32 wear levelling mkfatfs
===========================

This tool creates ESP32 fatfs images with wear levelling support.

# Operation

## Requirements

This tool requires esp-idf to be [set up properly](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/).
If you do run into issues building the tool check that the environment variable `IDF_PATH` points to your installation directory of esp-idf.

**IMPORTANT**: At the time of writing this tool does not work with the current stable version (v3.1) of esp-idf. You must use the latest version from the master branch to compile this tool.


## Configuration

At the moment configuration is done via the file `sdkconfig/sdkconfig.h`. I plan on adding a menuconfig though.
See section Limitations at the end of this docuemnt for limits on valid configuration values.

**IMPORTANT**: Do not forget to place your partition table in the file `partitions.csv`


## Building 

Building the tool should be straight forward. Just type `make`

This will create the tool binary called `mkfatfs`


## Usage

To build a fatfs image with wear levelling support just place all files you want to have in you fatfs in a directory and run

`./mkfatfs -c <dir name here> fatfs.img`

This will create the file `fatfs.img` containing the fatfs image.

To flash the image to your ESP run

```
python2 "$IDF_PATH"/components/esptool_py/esptool/esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 230400 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB 0x130000 fatfs.img
```

Depending on your partition layout you might have to update the address argument in above command

### List of options
```
Usage: ./mkfatfs [-c <fatfs directory>] [-t <partition table>] [-l <partition label>] <fatfs image name>
Options:
	 -c <fatfs directory>	Set directory to build fatfs from to <fatfs directory>. Defaults to 'image'
	 -t <partition table>	Set file to read partition table from to <partition table>. Defaults to 'partition_table.bin'
	 -l <partition label>	Set label of partition from partition table to use to <partition label>. Defaults to 'storage'
```

# Limitations

Currently this tool supports only sector sizes of 4096 because the flash mocking code of esp-idf is not suitable for other sector sizes
