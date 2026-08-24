# Standalone build + run of the Godot-free recovered algorithm (no Godot needed).
CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -I..
CORE = ../vcb_pipeline.c ../vcb_vec.c ../vcb_classifier.c ../vcb_resolver.c ../vcb_model.c ../vcb_sim.c ../vcb_vdisplay.c ../vcb_vmem.c ../vcb_bus.c
SRC = $(CORE) core_test.c

test:
	$(CC) $(CFLAGS) $(SRC) -o core_test
	./core_test

# Differential fuzzer: compiles thousands of random circuits through the real
# front-end and checks the event-driven vcb_sim_tick against the synchronous
# vcb_sim_tick_sync on every tick. A divergence is a scheduler bug (the class of
# "a trace moves when it shouldn't"). Runs 8000 iterations by default: `make fuzz`
# or `./fuzz_sim <iters>`.
fuzz:
	$(CC) $(CFLAGS) -O2 $(CORE) fuzz_sim.c -o fuzz_sim
	./fuzz_sim 8000

clean:
	rm -f core_test fuzz_sim
