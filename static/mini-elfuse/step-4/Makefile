CC = clang
CROSS = aarch64-linux-gnu-
BUILD = build

all: $(BUILD)/mini-elfuse $(BUILD)/hello $(BUILD)/null

$(BUILD):
	mkdir -p $@

$(BUILD)/mini-elfuse: mini-elfuse.c vectors.S entitlements.plist | $(BUILD)
	$(CC) -O2 -Wall -o $@ mini-elfuse.c vectors.S -framework Hypervisor
	codesign --entitlements entitlements.plist -f -s - $@

$(BUILD)/%: %.S | $(BUILD)
	$(CROSS)gcc -nostdlib -static -o $@ $<

check: all
	test "$$($(BUILD)/mini-elfuse $(BUILD)/hello)" = hello
	$(BUILD)/mini-elfuse $(BUILD)/null 2>&1 | grep 'FAR_EL1 0x0,'

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
