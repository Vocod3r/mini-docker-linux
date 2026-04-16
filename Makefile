# Makefile — mini-docker-linux
# No kernel module — /proc based monitoring

CC     := gcc
CFLAGS := -Wall -O2 -g -Wno-format-truncation -Wno-stringop-truncation \
          -Wno-unused-result -Wno-implicit-function-declaration
LIBS   := -lpthread

BINS := engine cpu_hog io_pulse memory_hog

.PHONY: all ci clean test

all: $(BINS)

# engine links engine.c + monitor.c together
engine: engine.c monitor.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ engine.c monitor.c $(LIBS)

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o $@ $<

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o $@ $<

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o $@ $<

ci: all
	@./engine 2>&1 | head -3 ; true
	@echo "CI build OK"

test: all
	@echo "=== usage ===" && ./engine 2>&1 | head -5 ; true
	@echo "=== supervisor not running ===" && ./engine ps 2>&1 ; true

clean:
	rm -f $(BINS) engine.log
	rm -rf /tmp/container_swap /tmp/mini_runtime.sock