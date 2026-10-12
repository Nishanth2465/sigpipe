CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDLIBS  = -pthread -lm
SRC     = src/main.c src/ring.c src/fir.c src/wav.c
HDR     = src/ring.h src/fir.h src/wav.h

.PHONY: all debug tsan test bench plot demo clean

all: sigpipe

sigpipe: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -pthread $(SRC) -o $@ $(LDLIBS)

# AddressSanitizer + UBSan: catches buffer overflows, use-after-free, UB
debug: $(SRC) $(HDR)
	$(CC) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined -pthread $(SRC) -o sigpipe_debug $(LDLIBS)

# ThreadSanitizer: catches data races between threads
tsan: $(SRC) $(HDR)
	$(CC) -O1 -g -Wall -Wextra -std=c11 -fsanitize=thread -pthread $(SRC) -o sigpipe_tsan $(LDLIBS)

# correctness: multithreaded output must match the single-threaded reference exactly
test: sigpipe
	./sigpipe -t 1 -n 1000000 -v
	./sigpipe -t 4 -n 1000000 -v
	./sigpipe -t 8 -n 1000003 -c 1000 -r 2 -v

bench: sigpipe
	./scripts/bench.sh

plot: sigpipe
	./sigpipe -t 4 -n 48000 -o results/signal.csv
	python3 scripts/plot.py

# real-audio demo: make a noisy recording, clean it, measure the improvement
demo: sigpipe
	python3 scripts/make_noisy_wav.py
	./sigpipe -i results/noisy.wav -w results/cleaned.wav -f 3500 -k 255 -v -o results/audio.csv
	python3 scripts/compare.py results/noisy.wav results/cleaned.wav results/clean.wav

clean:
	rm -f sigpipe sigpipe_debug sigpipe_tsan
