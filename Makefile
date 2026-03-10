# ── Configuración general ──
CC = gcc
CFLAGS = -Wall -Wextra -g

# ── Agrupamos los programas según cómo se compilan ──
NORMAL_BINS = prod cons
THREAD_BINS = prod_sem cons_sem opc1_threads opc2_multi opc3_pipeline

.PHONY: all clean

# ── Regla principal: compilar todo ──
all: $(NORMAL_BINS) $(THREAD_BINS)

# ── Regla para los programas SIN hilos (prod y cons) ──
$(NORMAL_BINS): %: %.c
	$(CC) $(CFLAGS) -o $@ $<

# ── Regla para los programas CON hilos/semáforos (-pthread) ──
$(THREAD_BINS): %: %.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

# ── Limpieza ──
clean:
	rm -f $(NORMAL_BINS) $(THREAD_BINS)
	
