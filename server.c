#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sysexits.h>
#include <unistd.h>

#include "cola.h"

const int no_sock = -1;
const int default_backlog = 5;
char * program_name;

Deque clientes;
pthread_mutex_t mutex_clientes = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_stdout = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_stack_readable = PTHREAD_COND_INITIALIZER;

void exit_usage(int exit_code) {
  fprintf(
    stderr,
    "Uso: %s -l <puerto_svr_s> -b <archivo_bitácora>\n"
    "Opciones:\n"
    "-l <puerto_svr_s>: Número de puerto local en el que el módulo central atenderá la llamada.\n"
    "-b <archivo_bitácora>: Nombre y dirección relativa o absoluta de un archivo de texto que realiza operaciones de bitácora.\n",
    program_name
  );
  exit(exit_code);
}

void encolar(int num) {
  int s = pthread_mutex_lock(&mutex_clientes);
  if (s != 0) {
    // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
    errno = s;
    perror("Error intentando entrar en la sección crítica del productor; pthread_mutex_lock: ");
    exit(EX_SOFTWARE);
  }

  { // Sección crítica:
    int * n = malloc(sizeof(int));
    *n = num;
    push_back_deque(clientes, (void *)n);
    pthread_cond_broadcast(&cond_stack_readable); // Como empilé un valor, ahora hay un dato disponible para leer en la pila.
  }

  s = pthread_mutex_unlock(&mutex_clientes);
  if (s != 0) {
    // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
    errno = s;
    perror("Error intentando salir de la sección crítica del productor; pthread_mutex_unlock: ");
    exit(EX_SOFTWARE);
  }
}

void * productor(void * arg) {
  (void)arg;

  for(int i = 0; i < 50; ++i) {
    encolar(i);
  }


  pthread_exit(NULL);
}

void * consumidor(void * arg) {
  (void)arg;

  while (1) {
    int s = pthread_mutex_lock(&mutex_clientes);
    if (s != 0) {
      // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
      errno = s;
      perror("Error intentando entrar en la sección crítica del consumidor; pthread_mutex_lock: ");
      exit(EX_SOFTWARE);
    }

    // Mientras NO haya datos en la pila…
    while (!(length_deque(clientes) > 0)) {
      // …luego, si no nos salimos, esperamos a que alguien inserte datos.  Cuando se entra a esta función, atómicamente se libera el mutex y se comienza a esperar por un signal sobre la condición.
      s = pthread_cond_wait(&cond_stack_readable, &mutex_clientes);
      if (s != 0) {
        // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
        errno = s;
        perror("ErROR intentando entrar en la sección crítica del consumidor; pthread_mutex_lock: ");
        exit(EX_SOFTWARE);
      }
      // Al ocurrir un signal sobre esta condición, esta función adquiere de nuevo el mutex y retorna.  Si otro consumidor no se nos adelantó, la condición del ciclo no se cumplirá (porque seremos los primeros en ver el nuevo dato disponible en la pila) y saldremos del ciclo.
    }

    { // Sección crítica:
      int * cliente = (int *)pop_front_deque(clientes);
      pthread_mutex_lock(&mutex_stdout);
      { // Sección crítica
        printf("Consumidor: desempilé %d.\n", *cliente);
        fflush(stdout);
      }
      pthread_mutex_unlock(&mutex_stdout);
      free(cliente);
    }

    s = pthread_mutex_unlock(&mutex_clientes);
    if (s != 0) {
      // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
      errno = s;
      perror("Error intentando salir de la sección crítica del consumidor; pthread_mutex_unlock: ");
      exit(EX_SOFTWARE);
    }
  }

  pthread_exit(NULL);

}

int main(int argc, char ** argv) {
  char opt;
  char * puerto   = NULL;
  char * bitacora = NULL;

  program_name = argv[0];

  while ((opt = getopt(argc, argv, "l:b:")) != -1) {
    switch (opt) {
      case 'l': puerto   = optarg; break;
      case 'b': bitacora = optarg; break;
      default:
        exit_usage(EX_USAGE);
    }
  }

  if (NULL == puerto) {
    fprintf(stderr, "El número de puerto local es obligatorio.\n");
    exit_usage(EX_USAGE);
  }
  if (NULL == bitacora) {
    fprintf(stderr, "El nombre del archivo bitácora es obligatorio.\n");
    exit_usage(EX_USAGE);
  }

  struct addrinfo hints;
  struct addrinfo * results;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_addr      = NULL;
  hints.ai_canonname = NULL;
  hints.ai_family    = AF_INET;
  hints.ai_flags     = AI_PASSIVE || AI_NUMERICSERV;
  hints.ai_next      = NULL;
  hints.ai_protocol  = getprotobyname("TCP")->p_proto;
  hints.ai_socktype  = SOCK_STREAM;

  {
    int i;
    if ((i = getaddrinfo(NULL, puerto, &hints, &results)) != 0) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(i));
      exit(EX_OSERR);
    }
  }

  int num_addrs = 0;
  for (struct addrinfo * result = results; result != NULL; result = result->ai_next) ++num_addrs;

  if (0 == num_addrs) {
    fprintf(stderr, "No se encontró ninguna manera de crear el servicio.\n");
    exit(EX_UNAVAILABLE);
  }

  int * sockfds;
  if ((sockfds = (int *)calloc(num_addrs, sizeof(int))) == NULL) {
    perror("calloc");
    exit(EX_OSERR);
  }

  int i = 0;
  int socks = 0;
  for (struct addrinfo * result = results; result != NULL; ++i, result = result->ai_next) {
    if ((sockfds[i] = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
      sockfds[i] = no_sock;
      continue;
    }

    if (bind(sockfds[i], result->ai_addr, result->ai_addrlen) == -1) {
      close(sockfds[i]);
      sockfds[i] = no_sock;
      continue;
    }

    if (listen(sockfds[i], default_backlog) == -1) {
      close(sockfds[i]);
      sockfds[i] = no_sock;
      continue;
    }

    ++socks;
  }

  freeaddrinfo(results);

  if (socks <= 0) {
    fprintf(stderr, "No se encontró ninguna manera de crear el servicio.\n");
    exit(EX_UNAVAILABLE);
  }

  i = 0;
  int j = 0;
  for (i = 0, j = 0; i < socks; ++i) if (sockfds[i] == no_sock) {
    if (j == 0) j = i+1;
    for (; j < num_addrs; ++j) if (sockfds[j] != no_sock) break;
    sockfds[i] = sockfds[j];
    ++j;
  }

  if ((sockfds = (int *)realloc(sockfds, socks*sizeof(int))) == NULL) {
    perror("realloc");
    exit(EX_OSERR);
  }

  for (i = 0; i < socks; ++i) if (fcntl(sockfds[i], F_SETFL, O_NONBLOCK) == -1) {
    perror("fcntl");
    exit(EX_OSERR);
  }

  int num_consumidores = 10;
  clientes = empty_deque();
  pthread_t tid_productor;
  pthread_t consumidores[num_consumidores];
  int s;

  // Crear los hilos productor y consumidor:
  s = pthread_create(&tid_productor, NULL, &productor, NULL);
  if (s != 0) {
    errno = s;
    perror("No fue posible crear hilo productor; pthread_create: ");
    exit(EX_OSERR);
  }

  for (int i = 0; i < num_consumidores; ++i) {
    s = pthread_create(&consumidores[i], NULL, &consumidor, NULL);
    if (s != 0) {
      errno = s;
      perror("No fue posible crear hilo consumidor; pthread_create: ");
      exit(EX_OSERR);
    }
  }

  // Esperar por la terminación de los hilos:
  s = pthread_join(tid_productor, NULL);
  if (s != 0) {
    errno = s;
    perror("No fue posible esperar por la terminación del hilo productor; pthread_join: ");
    exit(EX_OSERR);
  }

  for (int i = 0; i < num_consumidores; ++i) {
    s = pthread_join(consumidores[i], NULL);
    if (s != 0) {
      errno = s;
      perror("No fue posible esperar por la terminación del hilo consumidor; pthread_join: ");
      exit(EX_OSERR);
    }
  }

  exit(EX_OK);
}
