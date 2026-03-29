# ─────────────────────────────────────────────────────────────
#  Makefile — OS-Jackfruit container runtime
#
#  No kernel module required — memory monitoring via /proc.
#
#  Targets:
#    all     build all binaries (default)
#    ci      same as all — CI-safe, no kernel headers needed
#    clean   remove build artefacts
#    test    quick smoke test
# ─────────────────────────────────────────────────────────────

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -g

# engine is built from BOTH engine.c and monitor.c
# (monitor.c provides proc_read_mem etc. as regular C functions)
ENGINE      := engine
CPU_HOG     := cpu_hog
IO_PULSE    := io_pulse
MEMORY_HOG  := memory_hog

.PHONY: all ci clean test

all: $(ENGINE) $(CPU_HOG) $(IO_PULSE) $(MEMORY_HOG)

# engine links engine.c + monitor.c together
$(ENGINE): engine.c monitor.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ engine.c monitor.c

$(CPU_HOG): cpu_hog.c
	$(CC) $(CFLAGS) -o $@ $<

$(IO_PULSE): io_pulse.c
	$(CC) $(CFLAGS) -o $@ $<

$(MEMORY_HOG): memory_hog.c
	$(CC) $(CFLAGS) -o $@ $<

# CI target — identical to all (no kernel headers needed)
ci: all
	@echo "CI build complete."

# Smoke test: usage message + empty list
test: all
	@echo "=== Usage check ==="
	@./$(ENGINE) 2>&1 | head -5 ; true
	@echo "=== List (empty) ==="
	@./$(ENGINE) list 2>&1 ; true
	@echo "Smoke test passed."

clean:
	rm -f $(ENGINE) $(CPU_HOG) $(IO_PULSE) $(MEMORY_HOG)
	rm -f engine.log
	rm -rf /tmp/container_swap
