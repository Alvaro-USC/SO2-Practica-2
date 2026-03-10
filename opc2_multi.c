/*
 * opc2_multi.c  —  Ejercicio opcional 2: N productores y M consumidores
 *
 * Compilar : gcc -o opc2_multi opc2_multi.c -pthread
 * Ejecutar : ./opc2_multi <num_productores> <num_consumidores>
 *   Ejemplo: ./opc2_multi 3 2
 *
 * ── Descripción ──────────────────────────────────────────────────────────
 * Generalización del ejercicio 1 (threads) para un número arbitrario de
 * hilos productores y consumidores especificado por el usuario en tiempo
 * de ejecución.
 *
 * El número total de caracteres producidos es igual al total consumidos:
 *   · Se producen exactamente MAX_ITEMS = NUM_PROD * ITEMS_POR_PROD caracteres.
 *   · El trabajo de consumo se reparte entre los M consumidores de forma
 *     dinámica: cada consumidor consume hasta que se han procesado todos
 *     los ítems (señalizado mediante el semáforo 'fin_produccion' y un
 *     contador atómico 'consumidos_total').
 *
 * ── Semáforos ────────────────────────────────────────────────────────────
 *   vacias      : huecos libres en el buffer         (init = N)
 *   llenas      : elementos disponibles para consumir (init = 0)
 *   mutex_buf   : exclusión mutua sobre el buffer y top (init = 1)
 *   mutex_fin   : protege el contador consumidos_total   (init = 1)
 *   mutex_prod  : protege el fichero de entrada compartido entre productores
 *
 * ── Garantía de igualdad producidos == consumidos ────────────────────────
 * Cada productor i produce exactamente ITEMS_POR_PROD ítems (total fijo).
 * Cuando todos los productores terminan, cada uno hace sem_post(&llenas)
 * adicional por cada consumidor (señal de terminación ficticia = item '\0'),
 * de modo que los consumidores se desbloquean y comprueban el flag
 * 'produccion_terminada' para salir del bucle.
 * Al final se verifica que total_producidos == total_consumidos.
 *
 * ── Velocidades (fuera de la RC) ─────────────────────────────────────────
 *   Cada hilo (productor o consumidor) duerme un tiempo aleatorio entre
 *   0 y MAX_SLEEP segundos fuera de la región crítica para simular
 *   velocidades distintas y forzar situaciones de buffer lleno/vacío.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdatomic.h>

/* ── Constantes ──────────────────────────────────────────────────────── */
#define N              10      /* tamaño del buffer LIFO              */
#define ITEMS_POR_PROD 40      /* ítems que produce cada productor     */
#define MAX_SLEEP       2      /* sleep máximo fuera de RC (segundos)  */
#define INPUT_FILE     "input.txt"

/* ── Buffer compartido ───────────────────────────────────────────────── */
static char buf[N];
static int  top = 0;

/* ── Semáforos ───────────────────────────────────────────────────────── */
static sem_t vacias;       /* huecos libres                          */
static sem_t llenas;       /* elementos disponibles                  */
static sem_t mutex_buf;    /* exclusión mutua buffer + top           */
static sem_t mutex_fin;    /* exclusión mutua contador consumidos    */
static sem_t mutex_prod;   /* exclusión mutua lectura fichero        */

/* ── Variables de control de terminación ────────────────────────────── */
static atomic_int producidos_total = 0;  /* ítems producidos hasta ahora  */
static atomic_int consumidos_total = 0;  /* ítems consumidos hasta ahora  */
static int        total_a_producir = 0;  /* NUM_PROD * ITEMS_POR_PROD     */
static atomic_int produccion_fin   = 0;  /* flag: todos los prod terminaron*/

/* ── Fichero de entrada compartido entre productores ─────────────────── */
static FILE *fin_global = NULL;

/* ── Contadores de vocales por rol ───────────────────────────────────── */
static sem_t mutex_vocales_prod;
static sem_t mutex_vocales_cons;
static int prod_va=0,prod_ve=0,prod_vi=0,prod_vo=0,prod_vu=0;
static int cons_va=0,cons_ve=0,cons_vi=0,cons_vo=0,cons_vu=0;

/* ── Auxiliares ──────────────────────────────────────────────────────── */
static void contar_vocal(char c, int *a, int *e, int *i, int *o, int *u) {
    switch(c){
        case 'a':case 'A': (*a)++; break;
        case 'e':case 'E': (*e)++; break;
        case 'i':case 'I': (*i)++; break;
        case 'o':case 'O': (*o)++; break;
        case 'u':case 'U': (*u)++; break;
        default: break;
    }
}

static void print_buf(const char *tag) {
    printf("%s buf=[", tag);
    for (int i = 0; i < N; i++) printf("%c", buf[i]);
    printf("] top=%d\n", top);
}

/* ── Argumento para los hilos ────────────────────────────────────────── */
typedef struct { int id; } ThreadArg;

/* ══════════════════════════════════════════════════════════════════════
 * Hilo productor
 * ══════════════════════════════════════════════════════════════════════ */
static void *hilo_productor(void *arg)
{
    int id = ((ThreadArg*)arg)->id;
    unsigned int seed = (unsigned)time(NULL) ^ (unsigned)pthread_self();

    for (int i = 0; i < ITEMS_POR_PROD; i++) {

        /* ── produce_item(): leer carácter del fichero compartido ── */
        /* El fichero es compartido entre todos los productores → mutex */
        sem_wait(&mutex_prod);
        int c = fgetc(fin_global);
        if (c == EOF) { rewind(fin_global); c = fgetc(fin_global); }
        sem_post(&mutex_prod);

        char item = (char)c;

        /* Contabilizar vocal (con mutex propio) */
        sem_wait(&mutex_vocales_prod);
        contar_vocal(item, &prod_va,&prod_ve,&prod_vi,&prod_vo,&prod_vu);
        sem_post(&mutex_vocales_prod);

        printf("[PROD-%d] i=%2d  producido='%c'\n", id, i, item);

        /* Protocolo buffer */
        sem_wait(&vacias);
        sem_wait(&mutex_buf);

        /* ══ REGIÓN CRÍTICA ══ */
        buf[top] = item;
        top++;
        int vt = top;
        atomic_fetch_add(&producidos_total, 1);
        /* ══ FIN RC ══════════ */

        sem_post(&mutex_buf);
        sem_post(&llenas);

        printf("[PROD-%d] i=%2d  insertado  top=%d  producidos_total=%d\n",
               id, i, vt, (int)producidos_total);
        print_buf("[PROD] ");

        /* Sleep aleatorio fuera de RC */
        sleep(rand_r(&seed) % (MAX_SLEEP + 1));
    }

    printf("[PROD-%d] Terminado.\n", id);
    pthread_exit(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Hilo consumidor
 * ══════════════════════════════════════════════════════════════════════ */
static void *hilo_consumidor(void *arg)
{
    int id = ((ThreadArg*)arg)->id;
    unsigned int seed = (unsigned)time(NULL) ^ (unsigned)pthread_self() ^ 0xDEAD;

    while (1) {
        /*
         * Intentar consumir un ítem.
         * Cuando la producción ha terminado y ya no quedan ítems en el
         * buffer, los productores habrán hecho sem_post(&llenas) con un
         * ítem centinela '\0' para despertar a los consumidores bloqueados.
         */
        sem_wait(&llenas);

        /* Comprobar si era una señal de terminación */
        if (produccion_fin && consumidos_total >= total_a_producir) {
            /* Re-publicar la señal para que otros consumidores también salgan */
            sem_post(&llenas);
            break;
        }

        sem_wait(&mutex_buf);

        /* ══ REGIÓN CRÍTICA ══ */
        /* Verificar de nuevo dentro de la RC (puede haber cambiado) */
        if (top == 0) {
            sem_post(&mutex_buf);
            if (produccion_fin && consumidos_total >= total_a_producir) {
                sem_post(&llenas);
                break;
            }
            sem_post(&llenas); /* devolver el token que no usamos */
            continue;
        }
        top--;
        char item = buf[top];
        buf[top]  = '-';
        int vt    = top;
        atomic_fetch_add(&consumidos_total, 1);
        int ct = consumidos_total;
        /* ══ FIN RC ══════════ */

        sem_post(&mutex_buf);
        sem_post(&vacias);

        printf("[CONS-%d] retirado='%c'  top=%d  consumidos_total=%d\n",
               id, item, vt, ct);
        print_buf("[CONS] ");

        /* consume_item(): fuera de la RC */
        sem_wait(&mutex_vocales_cons);
        contar_vocal(item, &cons_va,&cons_ve,&cons_vi,&cons_vo,&cons_vu);
        sem_post(&mutex_vocales_cons);

        /* Sleep aleatorio fuera de RC */
        sleep(rand_r(&seed) % (MAX_SLEEP + 1));

        /* ¿Ya consumimos todo? */
        if (ct >= total_a_producir) {
            sem_post(&llenas); /* despertar a otros consumidores bloqueados */
            break;
        }
    }

    printf("[CONS-%d] Terminado.\n", id);
    pthread_exit(NULL);
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <num_productores> <num_consumidores>\n", argv[0]);
        fprintf(stderr, "  Ej: %s 3 2\n", argv[0]);
        return EXIT_FAILURE;
    }

    int np = atoi(argv[1]);   /* número de productores */
    int nc = atoi(argv[2]);   /* número de consumidores */

    if (np < 1 || nc < 1) {
        fprintf(stderr, "Error: num_productores y num_consumidores deben ser >= 1\n");
        return EXIT_FAILURE;
    }

    total_a_producir = np * ITEMS_POR_PROD;

    printf("[MAIN] %d productor(es), %d consumidor(es)\n", np, nc);
    printf("[MAIN] Total a producir/consumir: %d items\n\n", total_a_producir);

    /* 1. Inicializar buffer y fichero */
    memset(buf, '-', N);
    fin_global = fopen(INPUT_FILE, "r");
    if (!fin_global) { perror("fopen"); return EXIT_FAILURE; }

    /* 2. Inicializar semáforos */
    sem_init(&vacias,            0, N);
    sem_init(&llenas,            0, 0);
    sem_init(&mutex_buf,         0, 1);
    sem_init(&mutex_fin,         0, 1);
    sem_init(&mutex_prod,        0, 1);
    sem_init(&mutex_vocales_prod,0, 1);
    sem_init(&mutex_vocales_cons,0, 1);

    /* 3. Crear hilos productores */
    pthread_t *tid_p = malloc(np * sizeof(pthread_t));
    ThreadArg *arg_p = malloc(np * sizeof(ThreadArg));
    for (int i = 0; i < np; i++) {
        arg_p[i].id = i;
        pthread_create(&tid_p[i], NULL, hilo_productor, &arg_p[i]);
    }

    /* 4. Crear hilos consumidores */
    pthread_t *tid_c = malloc(nc * sizeof(pthread_t));
    ThreadArg *arg_c = malloc(nc * sizeof(ThreadArg));
    for (int i = 0; i < nc; i++) {
        arg_c[i].id = i;
        pthread_create(&tid_c[i], NULL, hilo_consumidor, &arg_c[i]);
    }

    /* 5. Esperar a que todos los productores terminen */
    for (int i = 0; i < np; i++) pthread_join(tid_p[i], NULL);

    /*
     * 6. Señalizar fin de producción a los consumidores.
     * Se activa el flag y se hace sem_post(&llenas) nc veces para
     * que cualquier consumidor bloqueado se despierte y pueda
     * comprobar el flag.
     */
    atomic_store(&produccion_fin, 1);
    for (int i = 0; i < nc; i++) sem_post(&llenas);

    /* 7. Esperar a que todos los consumidores terminen */
    for (int i = 0; i < nc; i++) pthread_join(tid_c[i], NULL);

    /* 8. Destruir semáforos */
    sem_destroy(&vacias);
    sem_destroy(&llenas);
    sem_destroy(&mutex_buf);
    sem_destroy(&mutex_fin);
    sem_destroy(&mutex_prod);
    sem_destroy(&mutex_vocales_prod);
    sem_destroy(&mutex_vocales_cons);

    fclose(fin_global);
    free(tid_p); free(arg_p);
    free(tid_c); free(arg_c);

    /* 9. Resumen */
    printf("\n[MAIN] === Resumen ===\n");
    printf("[MAIN] Producidos : %d\n", (int)producidos_total);
    printf("[MAIN] Consumidos : %d\n", (int)consumidos_total);

    printf("[MAIN] Vocales producidas: a=%d e=%d i=%d o=%d u=%d\n",
           prod_va,prod_ve,prod_vi,prod_vo,prod_vu);
    printf("[MAIN] Vocales consumidas: a=%d e=%d i=%d o=%d u=%d\n",
           cons_va,cons_ve,cons_vi,cons_vo,cons_vu);

    if (producidos_total == consumidos_total &&
        prod_va==cons_va && prod_ve==cons_ve && prod_vi==cons_vi &&
        prod_vo==cons_vo && prod_vu==cons_vu)
        printf("[MAIN] OK: producidos == consumidos, vocales coinciden.\n");
    else
        printf("[MAIN] ERROR: discrepancia entre producidos y consumidos.\n");

    return 0;
}
