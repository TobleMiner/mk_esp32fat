COMPONENT := mkfatfs

COMPONENT_LIB := lib$(COMPONENT).a

STUBS_LIB_DIR := $(IDF_PATH)/components/spi_flash/sim/stubs
STUBS_LIB_BUILD_DIR := $(STUBS_LIB_DIR)/build
STUBS_LIB := libstubs.a

SPI_FLASH_SIM_DIR := $(IDF_PATH)/components/spi_flash/sim
SPI_FLASH_SIM_BUILD_DIR := $(SPI_FLASH_SIM_DIR)/build
SPI_FLASH_SIM_LIB := libspi_flash.a

WEAR_LEVELLING_DIR := $(IDF_PATH)/components/wear_levelling/test_wl_host
WEAR_LEVELLING_BUILD_DIR := $(WEAR_LEVELLING_DIR)/build
WEAR_LEVELLING_LIB := libwl.a

FATFS_DIR := $(IDF_PATH)/components/fatfs/test_fatfs_host
FATFS_BUILD_DIR := $(FATFS_DIR)/build
FATFS_LIB := libfatfs.a

include Makefile.files

all: mkfatfs

# TODO Exterm^wEliminate
# SDKCONFIG := sdkconfig

ifndef SDKCONFIG
SDKCONFIG_DIR := $(dir $(realpath sdkconfig/sdkconfig.h))
SDKCONFIG := $(SDKCONFIG_DIR)sdkconfig.h
else
SDKCONFIG_DIR := $(dir $(realpath $(SDKCONFIG)))
endif

INCLUDE_FLAGS := $(addprefix -I, $(INCLUDE_DIRS) $(SDKCONFIG_DIR) $(IDF_PATH)/tools/catch)

CPPFLAGS += $(INCLUDE_FLAGS) -g -m32
CXXFLAGS += $(INCLUDE_FLAGS) -std=c++11 -g -m32

# Build libraries that this component is dependent on
$(STUBS_LIB_BUILD_DIR)/$(STUBS_LIB): force
	$(MAKE) -C $(STUBS_LIB_DIR) lib SDKCONFIG=$(SDKCONFIG)

$(SPI_FLASH_SIM_BUILD_DIR)/$(SPI_FLASH_SIM_LIB): force
	$(MAKE) -C $(SPI_FLASH_SIM_DIR) lib SDKCONFIG=$(SDKCONFIG)

$(WEAR_LEVELLING_BUILD_DIR)/$(WEAR_LEVELLING_LIB): force
	$(MAKE) -C $(WEAR_LEVELLING_DIR) lib SDKCONFIG=$(SDKCONFIG)

$(FATFS_BUILD_DIR)/$(FATFS_LIB): force
	$(MAKE) -C $(FATFS_DIR) lib SDKCONFIG=$(SDKCONFIG)

# Create target for building this component as a library
CFILES := $(filter %.c, $(SOURCE_FILES))
CPPFILES := $(filter %.cpp, $(SOURCE_FILES))

CTARGET = ${2}/$(patsubst %.c,%.o,$(notdir ${1}))
CPPTARGET = ${2}/$(patsubst %.cpp,%.o,$(notdir ${1}))

ifndef BUILD_DIR
BUILD_DIR := build
endif

OBJ_FILES := $(addprefix $(BUILD_DIR)/, $(filter %.o, $(notdir $(SOURCE_FILES:.cpp=.o) $(SOURCE_FILES:.c=.o))))

define COMPILE_C
$(call CTARGET, ${1}, $(BUILD_DIR)) : ${1} $(SDKCONFIG)
	echo $(call CTARGET, ${1}, $(BUILD_DIR))
	mkdir -p $(BUILD_DIR) 
	echo precompile
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $(call CTARGET, ${1}, $(BUILD_DIR)) ${1}
	echo postcompile
endef

define COMPILE_CPP
$(call CPPTARGET, ${1}, $(BUILD_DIR)) : ${1} $(SDKCONFIG)
	echo $(call CPPTARGET, ${1}, $(BUILD_DIR))
	mkdir -p $(BUILD_DIR) 
	echo precompile
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $(call CPPTARGET, ${1}, $(BUILD_DIR)) ${1}
	echo postcompile
endef

$(BUILD_DIR)/$(COMPONENT_LIB): $(OBJ_FILES) $(SDKCONFIG)
	mkdir -p $(BUILD_DIR)
	echo "ar in: $@"
	echo "ar out: $^"
	$(AR) rcs $@ $^

lib: $(BUILD_DIR)/$(COMPONENT_LIB)

$(foreach cfile, $(CFILES), $(eval $(call COMPILE_C, $(cfile))))
$(foreach cxxfile, $(CPPFILES), $(eval $(call COMPILE_CPP, $(cxxfile))))

# Create target for building this component as a standalone binary
BIN_SOURCE_FILES = \
	main.c \

BIN_OBJ_FILES = $(filter %.o, $(TEST_SOURCE_FILES:.cpp=.o) $(TEST_SOURCE_FILES:.c=.o))

mkfatfs: lib $(BIN_OBJ_FILES) $(WEAR_LEVELLING_BUILD_DIR)/$(WEAR_LEVELLING_LIB) $(SPI_FLASH_SIM_BUILD_DIR)/$(SPI_FLASH_SIM_LIB) $(STUBS_LIB_BUILD_DIR)/$(STUBS_LIB) $(FATFS_BUILD_DIR)/$(FATFS_LIB) partition_table.bin $(SDKCONFIG)
	g++ $(LDFLAGS) $(CXXFLAGS) -o $@  $(BIN_OBJ_FILES) -L$(BUILD_DIR) -l:$(COMPONENT_LIB) -L$(WEAR_LEVELLING_BUILD_DIR) -l:$(WEAR_LEVELLING_LIB) -L$(SPI_FLASH_SIM_BUILD_DIR) -l:$(SPI_FLASH_SIM_LIB) -L$(STUBS_LIB_BUILD_DIR) -l:$(STUBS_LIB) -L$(FATFS_BUILD_DIR) -l:$(FATFS_LIB)

# Create other necessary targets
partition_table.bin: partitions.csv
	python $(IDF_PATH)/components/partition_table/gen_esp32part.py --verify $< $@

force:

# Create target to cleanup files
clean:
	$(MAKE) -C $(STUBS_LIB_DIR) clean
	$(MAKE) -C $(SPI_FLASH_SIM_DIR) clean
	$(MAKE) -C $(WEAR_LEVELLING_DIR) clean
	rm -f $(OBJ_FILES) $(BIN_OBJ_FILES) mkfatfs $(COMPONENT_LIB) partition_table.bin

.PHONY: all mkfatfs force
