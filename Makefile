# Koth Browser Engine - build bare-metal
CC      ?= gcc
CFLAGS  := -O3 -march=native -mtune=native -fno-math-errno -fomit-frame-pointer -flto -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -D_GNU_SOURCE -pthread
LDFLAGS := -flto -O3 -pthread -lX11 -lz -ldl -lm

SRC := src/util.c src/dom.c src/css.c src/net.c src/font.c src/raster.c src/img.c src/engine.c src/ui.c
OBJ := $(SRC:.c=.o)

koth: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	@echo "==> binario koth pronto"

%.o: %.c src/koth.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) koth

.PHONY: clean
