NOEXT_FILES := $(shell find . -maxdepth 1 -type f ! -name "*.*" \
	! -name "Makefile" ! -name "makefile" ! -name "GNUmakefile" -printf '%f\n')

APX_PROGRAMS := $(patsubst %.apx,%,$(wildcard *.apx))
CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

.PHONY: compiler clean list

compiler:
	$(CC) $(CFLAGS) apxc.c -o apxc

list:
	@echo "Dateien ohne Endung:"
	@for f in $(NOEXT_FILES); do echo "  $$f"; done

clean:
	@echo "Lösche Compiler und erkannte APX-Ausgaben..."
	@rm -f -- apxc
	@for f in $(APX_PROGRAMS); do \
		if [ -f "$$f" ]; then \
			echo "  rm $$f"; \
			rm -f -- "$$f"; \
		fi; \
	done
	@echo "Fertig."