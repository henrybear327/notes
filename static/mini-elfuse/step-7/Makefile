CC = clang
CROSS = aarch64-linux-gnu-
BUILD = build

all: $(BUILD)/mini-elfuse $(BUILD)/hello $(BUILD)/null $(BUILD)/printf \
     $(BUILD)/files $(BUILD)/mmap

$(BUILD):
	mkdir -p $@

$(BUILD)/mini-elfuse: mini-elfuse.c vectors.S entitlements.plist | $(BUILD)
	$(CC) -O2 -Wall -o $@ mini-elfuse.c vectors.S -framework Hypervisor
	codesign --entitlements entitlements.plist -f -s - $@

$(BUILD)/%: %.S | $(BUILD)
	$(CROSS)gcc -nostdlib -static -o $@ $<

$(BUILD)/%: %.c | $(BUILD)
	$(CROSS)gcc -static -O2 -o $@ $<

check: all
	test "$$($(BUILD)/mini-elfuse $(BUILD)/hello)" = hello
	$(BUILD)/mini-elfuse $(BUILD)/null 2>&1 | grep 'FAR_EL1 0x0,'
	test "$$($(BUILD)/mini-elfuse $(BUILD)/printf)" = "hello, world"
	cd $(BUILD) && test "$$(./mini-elfuse ./files note.txt)" = \
	    "$$(printf 'note.txt: 27 bytes\nwritten by a Linux program')"
	test "$$($(BUILD)/mini-elfuse $(BUILD)/mmap)" = \
	    "$$(printf 'sum 133693440\nELF')"

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
