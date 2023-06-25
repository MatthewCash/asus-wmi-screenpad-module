obj-m := asus_wmi_screenpad.o

CWD := $(shell pwd)
SRC_DIR := $(CWD)/src/
OBJ_DIR := $(CWD)/obj/
KERNEL := $(KERNEL_MODULES)/build


.PHONY: all
all:
	ln -sf $(CWD)/Makefile $(SRC_DIR)
	make -C $(KERNEL) M=$(OBJ_DIR) src=$(SRC_DIR) modules
	rm $(SRC_DIR)Makefile

.PHONY: clean
clean:
	rm -rf $(OBJ_DIR)*

# Create directories for objects
$(OBJ_DIR):
	mkdir -p $@
