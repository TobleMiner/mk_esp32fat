ESP32 wear leveling mkfatfs
===========================

This tool creates ESP32 fatfs images with wear levelling support.

# Usage

## Requirements

This tool requires esp-idf to be [set up properly](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/)
If you do run into issues building the tool check that the environment variable `IDF_PATH` points to your installation directory of esp-idf


## Configuration

At the moment configuration is done via the file `sdkconfig/sdkconfig.h`. I plan on adding a menuconfig though.
See section Limitations at the end of this docuemnt for limits on valid configuration values.

**IMPORATNT**: Do not forget to place your partition table in the file `partitions.csv`


## Building 

Building the tool should be striaght forward. Just type `make`

This will create the tool binary called `mkfatfs`


## Usage

To build a fatfs image with wear leveling support just place all files you want to have in you fatfs in a directory and run

`./mkfatfs -c <dir name here> fatfs.img`

This will create the file `fatfs.img` containing the fatfs image.

To flash the image to your ESP run

```
python2 "$IDF_PATH"/components/esptool_py/esptool/esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 230400 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB 0x130000 fatfs.img
```

Depending on your partition layout you might have to update the value passed via the `--address` argument


# Limitations

Currently this tool supports only sector sizes of 4096 because the flash mocking code of esp-idf is not suitable for other sector sizes
