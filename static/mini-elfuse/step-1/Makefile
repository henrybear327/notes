CC = clang
CROSS = aarch64-linux-gnu-
BUILD = build

all: $(BUILD)/mini-elfuse $(BUILD)/hello

$(BUILD):
	mkdir -p $@

$(BUILD)/mini-elfuse: mini-elfuse.c | $(BUILD)
	$(CC) -O2 -Wall -o $@ mini-elfuse.c

$(BUILD)/%: %.S | $(BUILD)
	$(CROSS)gcc -nostdlib -static -o $@ $<

check: all
	$(BUILD)/mini-elfuse -v $(BUILD)/hello

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
