obj-m := asus_wmi_screenpad.o

CWD := $(shell pwd)
SRC_DIR := $(CWD)/src/
OBJ_DIR := $(CWD)/obj/
KERNEL := $(KERNEL_MODULES)/build

SRC_FILE_EXT = c
SRC_FILES = $(shell find $(SRC_DIR) -name '*.${SRC_FILE_EXT}')
SRCS_IN_OBJ = $(patsubst $(SRC_DIR)%, $(OBJ_DIR)%, $(SRC_FILES))

.PHONY: all
all: $(SRCS_IN_OBJ)
	ln -sf $(CWD)/Makefile $(OBJ_DIR)
	make -C $(KERNEL) M=$(OBJ_DIR) SRC_FILES="" modules
	rm $(OBJ_DIR)Makefile
	rm $(SRCS_IN_OBJ)

# Copy source files to output dir
$(SRCS_IN_OBJ):
	ln -s $(SRC_FILES) $@

.PHONY: clean
clean:
	rm -rf $(OBJ_DIR)*

# Create directories for objects
$(OBJ_DIR):
	mkdir -p $@
