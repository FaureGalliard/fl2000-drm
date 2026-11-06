# SPDX-License-Identifier: GPL-2.0
all: fl2000-emu fl2000-t

fl2000-emu: fl2000-emu.c
	$(CC) -O2 -g -Wall -Wno-unused-parameter -pthread -o $@ $<

fl2000-t: fl2000-t.c
	$(CC) -O2 -g -Wall -Wno-unused-parameter $< $(shell pkg-config --cflags --libs libdrm) -o $@

clean:
	rm -f fl2000-emu fl2000-t

.PHONY: all clean
