# Host test of pac_rewrite.c against the Python implementation's vectors.
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
VECTORS = vectors.txt

test: test_pac_rewrite $(VECTORS)
	./test_pac_rewrite $(VECTORS)

test_pac_rewrite: test_pac_rewrite.c pac_rewrite.c pac_rewrite.h
	$(CC) $(CFLAGS) -o $@ test_pac_rewrite.c pac_rewrite.c

$(VECTORS): ../../tools/pacstrip/insn.py ../../tools/pacstrip/vectors.py
	python3 -I ../../tools/pacstrip/vectors.py $@

clean:
	rm -f test_pac_rewrite $(VECTORS)

.PHONY: test clean
