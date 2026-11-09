# Host tests: pac_rewrite.c against the Python implementation's vectors, and
# pac_emulate.c (LDRAA/LDRAB + FJCVTZS) against the host FPU.
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
VECTORS = vectors.txt

test: test_pac_rewrite test_pac_emulate $(VECTORS)
	./test_pac_rewrite $(VECTORS)
	./test_pac_emulate

test_pac_rewrite: test_pac_rewrite.c pac_rewrite.c pac_rewrite.h
	$(CC) $(CFLAGS) -o $@ test_pac_rewrite.c pac_rewrite.c

test_pac_emulate: test_pac_emulate.c pac_emulate.c pac_emulate.h
	$(CC) $(CFLAGS) -o $@ test_pac_emulate.c pac_emulate.c -lm

$(VECTORS): ../../tools/pacstrip/insn.py ../../tools/pacstrip/vectors.py
	python3 -I ../../tools/pacstrip/vectors.py $@

clean:
	rm -f test_pac_rewrite test_pac_emulate $(VECTORS)

.PHONY: test clean
