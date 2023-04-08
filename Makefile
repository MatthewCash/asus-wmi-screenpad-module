obj-m := asus_wmi_screenpad.o

PWD := $(shell pwd)
OUTPUT := $(PWD)/obj
KERNEL := $(KERNEL_MODULES)/build

all:
	make -C $(KERNEL) M=$(OUTPUT) src=$(PWD) modules
clean:
	rm -rf $(OUTPUT)/*
