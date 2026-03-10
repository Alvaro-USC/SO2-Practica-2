/*
 * opc1_threads.c  —  Ejercicio opcional 1: productor-consumidor con THREADS
 *
 * Compilar : gcc -o opc1_threads opc1_threads.c -pthread
 * Ejecutar : ./opc1_threads
 *
 * ── Diferencias respecto a prod_sem.c / cons_sem.c ──────────────────────
 * Al usar hilos (pthread) en lugar de procesos, la memoria es compartida
 * de forma natural (mismo espacio de direcciones), por lo que NO se
 * necesita mmap() ni fichero de memoria compartida.
 *
 * Los semáforos también cambian: en lugar de semáforos POSIX nombrados
 * (sem_open / sem_unlink), se usan semáforos POSIX sin nombre
 * (sem_init / sem_destroy), que solo son visibles dentro del proceso.
 * Esto es más adecuado para hilos y evita dejar residuos en el kernel.
 *
 * ── Estructura ───────────────────────────────────────────────────────────
 * Un único programa lanza dos hilos: hilo_productor y hilo_consumidor.
 * Comparten el buffer buf[N], el índice top y los tres semáforos,
 * todos declarados como variables globales.
 *
 * ── Semáforos usados ─────────────────────────────────────────────────────
 *   vacias : inicializado a N → nº de huecos libres
 *   llenas : inicializado a 0 → nº de elementos listos para consumir
 *   mutex  : inicializado a 1 → exclusión mutua en la región crítica
 *
 * ── Velocidades (fuera de la RC) ─────────────────────────────────────────
 *   iter  0-29 : productor rápido, consumidor duerme 2s → buffer se llena
 *   iter 30-59 : productor duerme 2s, consumidor rápido → buffer se vacía
 *   iter 60-79 : ambos aleatorio 0-3s
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

/* ── Constantes ──────────────────────────────────────────────────────── */
#define N          10
#define MAX_ITER   80
#define INPUT_FILE "input.txt"

/* ── Buffer compartido (en memoria del proceso, no necesita mmap) ────── */
static char buf[N];   /* pila LIFO                            */
static int  top = 0;  /* índice de la cima (0=vacío, N=lleno) */

/*
 * Semáforos sin nombre (sem_t por valor, no puntero).
 * sem_init(&sem, 0, valor):
 *   · 2º arg = 0 → compartido entre hilos del mismo proceso
 *   · 2º arg = 1 → compartido entre procesos (requeriría memoria compartida)
 */
static sem_t vacias;  /* huecos libres    → inicializado a N */
static sem_t llenas;  /* elementos listos → inicializado a 0 */
static sem_t mutex;   /* exclusión mutua sobre el buffer     */
static sem_t mtx_log; /* exclusión mutua sobre la salida por pantalla
                       * Evita que las líneas de PROD y CONS se entrelacen.
                       * Se toma DESPUÉS de liberar mutex (nunca dentro de la
                       * RC del buffer) para no reducir la concurrencia. */

/* ── Contadores de vocales (uno por hilo, no necesitan mutex propio) ── */
static int prod_va=0, prod_ve=0, prod_vi=0, prod_vo=0, prod_vu=0;
static int cons_va=0, cons_ve=0, cons_vi=0, cons_vo=0, cons_vu=0;

/* ── Auxiliares ──────────────────────────────────────────────────────── */
static void contar_vocal_prod(char c) {
    switch(c) {
        case 'a': case 'A': prod_va++; break;
        case 'e': case 'E': prod_ve++; break;
        case 'i': case 'I': prod_vi++; break;
        case 'o': case 'O': prod_vo++; break;
        case 'u': case 'U': prod_vu++; break;
        default: break;
    }
}
static void contar_vocal_cons(char c) {
    switch(c) {
        case 'a': case 'A': cons_va++; break;
        case 'e': case 'E': cons_ve++; break;
        case 'i': case 'I': cons_vi++; break;
        case 'o': case 'O': cons_vo++; break;
        case 'u': case 'U': cons_vu++; break;
        default: break;
    }
}

static void print_buf(void) {
    printf("  buf=[");
    for (int i = 0; i < N; i++) printf("%c", buf[i]);
    printf("]  top=%d\n", top);
}

/* ── Hilo productor ──────────────────────────────────────────────────── */
static void *hilo_productor(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned)time(NULL) ^ (unsigned)pthread_self();

    FILE *fin = fopen(INPUT_FILE, "r");
    if (!fin) { perror("[PROD] fopen"); pthread_exit(NULL); }

    for (int iter = 0; iter < MAX_ITER; iter++) {

        /* produce_item(): leer carácter fuera de la RC */
        int c = fgetc(fin);
        if (c == EOF) { rewind(fin); c = fgetc(fin); }
        char item = (char)c;
        contar_vocal_prod(item);

        /* Protocolo de semáforos */
        sem_wait(&vacias);   /* esperar si buffer lleno  */
        sem_wait(&mutex);    /* entrar en región crítica */

        /* ══ REGIÓN CRÍTICA ══ */
        buf[top] = item;
        top++;
        int val_top = top;
        /* ══ FIN RC ══════════ */

        sem_post(&mutex);    /* salir de región crítica   */
        sem_post(&llenas);   /* avisar al consumidor      */

        /*
         * Tomar mtx_log DESPUÉS de liberar mutex para no bloquear
         * al consumidor mientras imprimimos. El valor val_top fue
         * capturado dentro de la RC, así que es coherente aunque
         * el consumidor actúe antes de que lleguemos aquí.
         */
        sem_wait(&mtx_log);
        printf("[PROD] iter=%2d  producido='%c'  →  insertado  top=%d\n",
               iter, item, val_top);
        print_buf();
        sem_post(&mtx_log);

        /* Sleep fuera de la RC para controlar velocidad */
        if      (iter < 30) { /* rápido: no duerme */ }
        else if (iter < 60) { sleep(2); }
        else                { sleep(rand_r(&seed) % 4); }
    }

    fclose(fin);
    printf("[PROD] Terminado.\n");
    pthread_exit(NULL);
}

/* ── Hilo consumidor ─────────────────────────────────────────────────── */
static void *hilo_consumidor(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned)time(NULL) ^ (unsigned)pthread_self() ^ 0xABCD;

    for (int iter = 0; iter < MAX_ITER; iter++) {

        /* Protocolo de semáforos */
        sem_wait(&llenas);   /* esperar si buffer vacío  */
        sem_wait(&mutex);    /* entrar en región crítica */

        /* ══ REGIÓN CRÍTICA ══ */
        top--;
        char item   = buf[top];
        buf[top]    = '-';
        int val_top = top;
        /* ══ FIN RC ══════════ */

        sem_post(&mutex);    /* salir de región crítica */
        sem_post(&vacias);   /* avisar al productor     */

        sem_wait(&mtx_log);
        printf("[CONS] iter=%2d  retirado='%c'   top=%d\n", iter, item, val_top);
        print_buf();
        sem_post(&mtx_log);

        /* consume_item(): fuera de la RC */
        contar_vocal_cons(item);

        /* Sleep fuera de la RC para controlar velocidad */
        if      (iter < 30) { sleep(2); }
        else if (iter < 60) { /* rápido: no duerme */ }
        else                { sleep(rand_r(&seed) % 4); }
    }

    printf("[CONS] Terminado.\n");
    pthread_exit(NULL);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    /* 1. Inicializar buffer */
    memset(buf, '-', N);

    /*
     * 2. Inicializar semáforos sin nombre.
     * sem_init(&sem, pshared, valor)
     *   pshared=0 → compartido solo entre hilos del mismo proceso
     */
    sem_init(&vacias,   0, N);  /* N huecos libres al inicio  */
    sem_init(&llenas,   0, 0);  /* 0 elementos al inicio      */
    sem_init(&mutex,    0, 1);  /* mutex buffer libre         */
    sem_init(&mtx_log,  0, 1);  /* mutex log libre            */

    printf("[MAIN] Lanzando productor y consumidor como hilos.\n");
    printf("[MAIN] Buffer N=%d, MAX_ITER=%d\n\n", N, MAX_ITER);

    /* 3. Crear hilos */
    pthread_t tid_prod, tid_cons;
    pthread_create(&tid_prod, NULL, hilo_productor,  NULL);
    pthread_create(&tid_cons, NULL, hilo_consumidor, NULL);

    /* 4. Esperar a que ambos terminen */
    pthread_join(tid_prod, NULL);
    pthread_join(tid_cons, NULL);

    /* 5. Destruir semáforos (equivalente a sem_unlink para sin-nombre) */
    sem_destroy(&vacias);
    sem_destroy(&llenas);
    sem_destroy(&mutex);
    sem_destroy(&mtx_log);

    /* 6. Imprimir vocales */
    printf("\n[MAIN] === Vocales producidas ===\n");
    printf("  a/A=%d  e/E=%d  i/I=%d  o/O=%d  u/U=%d\n",
           prod_va, prod_ve, prod_vi, prod_vo, prod_vu);
    printf("[MAIN] === Vocales consumidas ===\n");
    printf("  a/A=%d  e/E=%d  i/I=%d  o/O=%d  u/U=%d\n",
           cons_va, cons_ve, cons_vi, cons_vo, cons_vu);

    /* Verificar que se consumió exactamente lo producido */
    if (prod_va==cons_va && prod_ve==cons_ve && prod_vi==cons_vi &&
        prod_vo==cons_vo && prod_vu==cons_vu)
        printf("[MAIN] OK: vocales producidas == vocales consumidas.\n");
    else
        printf("[MAIN] ERROR: discrepancia en vocales.\n");

    return 0;
}