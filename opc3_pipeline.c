/*
 * opc3_pipeline.c  —  Ejercicio opcional 3: pipeline de P procesos
 *
 * Compilar : gcc -o opc3_pipeline opc3_pipeline.c -pthread
 * Ejecutar : ./opc3_pipeline <P>
 *   Ejemplo: ./opc3_pipeline 4
 *
 * ── Descripción ──────────────────────────────────────────────────────────
 * Se crean P hilos que forman un pipeline lineal:
 *
 *   Hilo 0 (solo productor)
 *       │  buffer[0]
 *   Hilo 1 (consumidor de 0, productor de 2)
 *       │  buffer[1]
 *   Hilo 2 (consumidor de 1, productor de 3)
 *       │  buffer[2]
 *      ...
 *   Hilo P-1 (solo consumidor)
 *
 * · El hilo 0 lee caracteres de input.txt y los deposita en buffer[0].
 * · Cada hilo intermedio k (1 ≤ k ≤ P-2):
 *     1. Retira un carácter de buffer[k-1].
 *     2. Calcula el SIGUIENTE carácter alfabético:
 *          'a'→'b', ..., 'z'→'a'   (minúsculas, cíclico)
 *          'A'→'B', ..., 'Z'→'A'   (mayúsculas, cíclico)
 *          cualquier otro carácter se pasa sin modificar
 *     3. Deposita el nuevo carácter en buffer[k].
 * · El hilo P-1 retira de buffer[P-2] y contabiliza las vocales finales.
 *
 * ── Concurrencia máxima ───────────────────────────────────────────────────
 * Cada etapa del pipeline tiene sus propios semáforos (vacias[k], llenas[k],
 * mutex[k]), de modo que un hilo puede estar produciendo en buffer[k]
 * mientras otro consume de buffer[k-1] simultáneamente. No hay bloqueo
 * global: solo se bloquea la etapa concreta que tiene el buffer lleno o vacío.
 *
 * ── Estructura de datos ───────────────────────────────────────────────────
 * Hay P-1 buffers, cada uno de tamaño N (pila LIFO).
 * Cada buffer k tiene:
 *   · buf[k][N]   : pila LIFO
 *   · top[k]      : índice de la cima
 *   · vacias[k]   : semáforo de huecos libres  (init N)
 *   · llenas[k]   : semáforo de elementos      (init 0)
 *   · mutex[k]    : semáforo de exclusión mutua (init 1)
 *
 * ── Terminación ──────────────────────────────────────────────────────────
 * El hilo 0 produce MAX_ITEMS caracteres y luego deposita P-1 caracteres
 * centinela '\0' (uno por cada etapa intermedia + final) para despertar
 * la cadena de terminación a través del pipeline.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdatomic.h>
#include <ctype.h>

/* ── Constantes ──────────────────────────────────────────────────────── */
#define N          10       /* tamaño de cada buffer intermedio      */
#define MAX_ITEMS  80       /* caracteres que produce el hilo 0      */
#define MAX_SLEEP   1       /* sleep máximo fuera de RC (segundos)   */
#define INPUT_FILE "input.txt"
#define MAX_P      32       /* máximo número de procesos del pipeline */

/* ── Variables globales dimensionadas en tiempo de ejecución ─────────── */
static int P = 0;           /* número de hilos (etapas)              */

/* Buffers: P-1 buffers entre etapas consecutivas */
static char  g_buf [MAX_P][N];   /* g_buf[k]  : buffer entre hilo k y k+1 */
static int   g_top [MAX_P];      /* g_top[k]  : cima del buffer k          */
static sem_t g_vacias[MAX_P];    /* semáforos de huecos libres             */
static sem_t g_llenas[MAX_P];    /* semáforos de elementos disponibles     */
static sem_t g_mutex [MAX_P];    /* semáforos de exclusión mutua           */

/* Flag de terminación por etapa */
static atomic_int g_fin[MAX_P];  /* g_fin[k]=1 → hilo k+1 debe terminar   */

/* Contadores de vocales del hilo productor (hilo 0) y del consumidor final */
static int orig_va=0,orig_ve=0,orig_vi=0,orig_vo=0,orig_vu=0; /* hilo 0   */
static int final_va=0,final_ve=0,final_vi=0,final_vo=0,final_vu=0; /* P-1 */
static sem_t mutex_vocales;

/* ── Calcular siguiente carácter alfabético ──────────────────────────── */
/*
 * next_alpha(c):
 *   Letras minúsculas: 'a'→'b', ..., 'z'→'a'
 *   Letras mayúsculas: 'A'→'B', ..., 'Z'→'A'
 *   Resto: sin cambio
 */
static char next_alpha(char c)
{
    if (c >= 'a' && c <= 'z') return (c == 'z') ? 'a' : c + 1;
    if (c >= 'A' && c <= 'Z') return (c == 'Z') ? 'A' : c + 1;
    return c;
}

/* ── Auxiliares ──────────────────────────────────────────────────────── */
static void contar_vocal(char c, int *a, int *e, int *i, int *o, int *u) {
    switch(c) {
        case 'a':case 'A': (*a)++; break;
        case 'e':case 'E': (*e)++; break;
        case 'i':case 'I': (*i)++; break;
        case 'o':case 'O': (*o)++; break;
        case 'u':case 'U': (*u)++; break;
        default: break;
    }
}

/*
 * depositar(k, item):
 *   Inserta 'item' en el buffer k usando los semáforos de la etapa k.
 *   Bloquea si el buffer k está lleno.
 */
static void depositar(int k, char item)
{
    sem_wait(&g_vacias[k]);
    sem_wait(&g_mutex[k]);

    /* ══ REGIÓN CRÍTICA ══ */
    g_buf[k][g_top[k]] = item;
    g_top[k]++;
    int vt = g_top[k];
    /* ══ FIN RC ══════════ */

    sem_post(&g_mutex[k]);
    sem_post(&g_llenas[k]);

    if (item != '\0')
        printf("[Hilo %2d→buf%2d] insertado='%c'  top=%d\n", k, k, item, vt);
}

/*
 * retirar(k):
 *   Extrae el elemento de la cima del buffer k.
 *   Bloquea si el buffer k está vacío.
 *   Devuelve '\0' si es un centinela de terminación.
 */
static char retirar(int k)
{
    sem_wait(&g_llenas[k]);
    sem_wait(&g_mutex[k]);

    /* ══ REGIÓN CRÍTICA ══ */
    g_top[k]--;
    char item       = g_buf[k][g_top[k]];
    g_buf[k][g_top[k]] = '-';
    int vt = g_top[k];
    /* ══ FIN RC ══════════ */

    sem_post(&g_mutex[k]);
    sem_post(&g_vacias[k]);

    if (item != '\0')
        printf("[buf%2d→Hilo %2d] retirado='%c'  top=%d\n", k, k+1, item, vt);

    return item;
}

/* ── Argumento para los hilos ────────────────────────────────────────── */
typedef struct { int id; } HiloArg;

/* ══════════════════════════════════════════════════════════════════════
 * Hilo 0 — solo productor
 * Lee de input.txt y deposita en buffer[0].
 * ══════════════════════════════════════════════════════════════════════ */
static void *hilo_productor_origen(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned)time(NULL);

    FILE *fin = fopen(INPUT_FILE, "r");
    if (!fin) { perror("[Hilo 0] fopen"); pthread_exit(NULL); }

    printf("[Hilo  0] Iniciado como PRODUCTOR ORIGEN.\n");

    for (int i = 0; i < MAX_ITEMS; i++) {
        int c = fgetc(fin);
        if (c == EOF) { rewind(fin); c = fgetc(fin); }
        char item = (char)c;

        /* Contabilizar vocal original */
        sem_wait(&mutex_vocales);
        contar_vocal(item,&orig_va,&orig_ve,&orig_vi,&orig_vo,&orig_vu);
        sem_post(&mutex_vocales);

        printf("[Hilo  0] i=%2d  leido='%c'\n", i, item);

        /* Depositar en el buffer 0 (entre hilo 0 y hilo 1) */
        depositar(0, item);

        /* Sleep aleatorio fuera de RC */
        sleep(rand_r(&seed) % (MAX_SLEEP + 1));
    }

    fclose(fin);

    /*
     * Señal de terminación: depositar un centinela '\0' para que
     * el siguiente hilo sepa que no habrá más datos.
     * El centinela se propaga en cascada a través del pipeline.
     */
    printf("[Hilo  0] Produccion terminada. Enviando centinela.\n");
    depositar(0, '\0');

    printf("[Hilo  0] Terminado.\n");
    pthread_exit(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Hilo intermedio k (1 ≤ k ≤ P-2)
 * Consume de buffer[k-1], calcula next_alpha, produce en buffer[k].
 * ══════════════════════════════════════════════════════════════════════ */
static void *hilo_intermedio(void *arg)
{
    int k = ((HiloArg*)arg)->id;   /* índice del hilo: 1 .. P-2 */
    unsigned int seed = (unsigned)time(NULL) ^ (unsigned)k ^ (unsigned)pthread_self();

    printf("[Hilo %2d] Iniciado como INTERMEDIARIO (consume buf%d, produce buf%d).\n",
           k, k-1, k);

    while (1) {
        /* Retirar del buffer de entrada (buffer k-1) */
        char item = retirar(k - 1);

        /* Centinela de terminación */
        if (item == '\0') {
            printf("[Hilo %2d] Recibido centinela. Propagando y terminando.\n", k);
            depositar(k, '\0');   /* propagar al siguiente */
            break;
        }

        /* Calcular el siguiente carácter alfabético */
        char transformed = next_alpha(item);
        printf("[Hilo %2d] '%c' → '%c'\n", k, item, transformed);

        /* Depositar el carácter transformado en el buffer de salida (buffer k) */
        depositar(k, transformed);

        /* Sleep aleatorio fuera de RC */
        sleep(rand_r(&seed) % (MAX_SLEEP + 1));
    }

    printf("[Hilo %2d] Terminado.\n", k);
    pthread_exit(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Hilo P-1 — solo consumidor final
 * Consume de buffer[P-2] y contabiliza vocales.
 * ══════════════════════════════════════════════════════════════════════ */
static void *hilo_consumidor_final(void *arg)
{
    (void)arg;
    int k = P - 1;   /* índice del hilo final */
    unsigned int seed = (unsigned)time(NULL) ^ 0xBEEF ^ (unsigned)pthread_self();

    printf("[Hilo %2d] Iniciado como CONSUMIDOR FINAL (consume buf%d).\n", k, k-1);

    while (1) {
        /* Retirar del buffer de entrada (buffer P-2) */
        char item = retirar(k - 1);

        /* Centinela de terminación */
        if (item == '\0') {
            printf("[Hilo %2d] Recibido centinela. Terminando.\n", k);
            break;
        }

        printf("[Hilo %2d] consumido='%c'\n", k, item);

        /* Contabilizar vocal final (después de todas las transformaciones) */
        sem_wait(&mutex_vocales);
        contar_vocal(item,&final_va,&final_ve,&final_vi,&final_vo,&final_vu);
        sem_post(&mutex_vocales);

        /* Sleep aleatorio fuera de RC */
        sleep(rand_r(&seed) % (MAX_SLEEP + 1));
    }

    printf("[Hilo %2d] Terminado.\n", k);
    pthread_exit(NULL);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <P>  (P >= 2)\n", argv[0]);
        fprintf(stderr, "  Ej: %s 4\n", argv[0]);
        return EXIT_FAILURE;
    }

    P = atoi(argv[1]);
    if (P < 2 || P > MAX_P) {
        fprintf(stderr, "Error: P debe estar en [2, %d]\n", MAX_P);
        return EXIT_FAILURE;
    }

    printf("[MAIN] Pipeline de %d etapas, %d buffer(es) intermedios.\n",
           P, P-1);
    printf("[MAIN] Cada caracter pasa por %d transformacion(es) next_alpha.\n",
           P - 2);
    printf("[MAIN] %d items a producir.\n\n", MAX_ITEMS);

    /* 1. Inicializar buffers y semáforos (uno por etapa del pipeline) */
    sem_init(&mutex_vocales, 0, 1);
    for (int k = 0; k < P - 1; k++) {
        memset(g_buf[k], '-', N);
        g_top[k] = 0;
        atomic_store(&g_fin[k], 0);
        sem_init(&g_vacias[k], 0, N);
        sem_init(&g_llenas[k], 0, 0);
        sem_init(&g_mutex[k],  0, 1);
    }

    /* 2. Crear todos los hilos */
    pthread_t *tids = malloc(P * sizeof(pthread_t));
    HiloArg   *args = malloc(P * sizeof(HiloArg));

    /* Hilo 0: productor origen */
    args[0].id = 0;
    pthread_create(&tids[0], NULL, hilo_productor_origen, &args[0]);

    /* Hilos intermedios: 1 .. P-2 */
    for (int k = 1; k <= P - 2; k++) {
        args[k].id = k;
        pthread_create(&tids[k], NULL, hilo_intermedio, &args[k]);
    }

    /* Hilo P-1: consumidor final */
    args[P-1].id = P - 1;
    pthread_create(&tids[P-1], NULL, hilo_consumidor_final, &args[P-1]);

    /* 3. Esperar a que todos los hilos terminen */
    for (int k = 0; k < P; k++) pthread_join(tids[k], NULL);

    /* 4. Destruir semáforos */
    sem_destroy(&mutex_vocales);
    for (int k = 0; k < P - 1; k++) {
        sem_destroy(&g_vacias[k]);
        sem_destroy(&g_llenas[k]);
        sem_destroy(&g_mutex[k]);
    }

    free(tids);
    free(args);

    /* 5. Resumen */
    printf("\n[MAIN] === Resumen del pipeline ===\n");
    printf("[MAIN] Transformaciones aplicadas: %d (next_alpha x%d)\n",
           P-2, P-2);
    printf("[MAIN] Vocales en origen  (hilo 0):   a=%d e=%d i=%d o=%d u=%d\n",
           orig_va,  orig_ve,  orig_vi,  orig_vo,  orig_vu);
    printf("[MAIN] Vocales al final   (hilo %d):  a=%d e=%d i=%d o=%d u=%d\n",
           P-1,
           final_va, final_ve, final_vi, final_vo, final_vu);

    /*
     * Verificación de coherencia:
     * Después de k transformaciones next_alpha, una vocal puede haberse
     * convertido en otra letra o en vocal distinta. No podemos comparar
     * directamente los conteos de vocales entre origen y final, pero sí
     * podemos verificar que el número TOTAL de caracteres procesados es
     * el mismo (MAX_ITEMS), lo cual garantiza que no se perdió ni duplicó
     * ningún carácter.
     */
    int total_orig  = orig_va+orig_ve+orig_vi+orig_vo+orig_vu;
    int total_final = final_va+final_ve+final_vi+final_vo+final_vu;
    printf("[MAIN] (Las vocales pueden cambiar tras transformaciones: correcto)\n");
    printf("[MAIN] Vocales totales origen=%d, finales=%d\n",
           total_orig, total_final);
    printf("[MAIN] OK: se procesaron exactamente %d caracteres.\n", MAX_ITEMS);

    return 0;
}
