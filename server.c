/**
 * @file server.c
 * @author Gabriela Limonta 10-10385
 * @author John Delgado 10-10196
 *
 * Contiene la implementación del servidor SVR.
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include "evento.h"
#include "curl/include/curl/curl.h"

#define min(x,y) ((x) < (y) ? (x) : (y))

const int no_sock = -1;             // Indica no socket.
const int default_backlog = 5;      // Longitud de la cola.
char * program_name;                // Nombre del programa.
char * to = "<glimonta@gmail.com>"; // Dirección de correo a la que se envian las alertas.

char * puerto   = NULL; // Puerto del servidor.
char * bitacora = NULL; // Nombre del archivo de la bitacora.
char * config   = NULL; // Nombre del archivo de configuración.

FILE * bitacora_file; // Archivo de la bitácora.
FILE * config_file;   // Archivo de configuración.

#define N_PATRONES 13
int patrones[N_PATRONES]; // Arreglo con los patrones que se buscaran para enviar alertas.

Deque clientes;                                                // Cola de clientes.
pthread_mutex_t mutex_clientes = PTHREAD_MUTEX_INITIALIZER;    // Mutex para la cola clientes.
pthread_mutex_t mutex_stdout = PTHREAD_MUTEX_INITIALIZER;      // Mutex para la stdout.
pthread_cond_t cond_stack_readable = PTHREAD_COND_INITIALIZER; //Condición que indica si se puede leer de la cola.

int numero_de_desconexiones = 0; // Contador de perdidas de conexiones

/**
 * Se encarga de imprimir un mensaje de error cuando el usuario
 * se equivoca en la invocación del SVR y abortar la ejecución del
 * programa con el mensaje de error indicado.
 * @param exit_code codigo de error con el que se saldrá del programa.
 */
void exit_usage(int exit_code) {
  fprintf(
    stderr,
    "Uso: %s -l <puerto_svr_s> -b <archivo_bitácora> [-c <archivo_configuración>]\n"
    "Opciones:\n"
    "-l <puerto_svr_s>: Número de puerto local en el que el módulo central atenderá la llamada.\n"
    "-b <archivo_bitácora>: Nombre y dirección relativa o absoluta de un archivo de texto que realiza operaciones de bitácora.\n"
    "-c <archivo_configuración>: Nombre y dirección relativa o absoluta de un archivo de texto que contiene la configuración del SVR.\n",
    program_name
  );
  exit(exit_code);
}

// Dirección de correo de la que se enviarán las alertas.
#define FROM "<10-10385@ldc.usb.ve>"

// String de formato para los correos de alerta que se llenará luego con la información adecuada.
static const char payload_text[] =
  "To: %s \r\n"
  "From: " FROM " (SVR)\r\n"
  "Subject: Alerta SVR! :(\r\n"
  "\r\n" /* empty line to divide headers from body, see RFC5322 */
  "Hubo una alerta en el ATM %d.\r\n"
  "Código de error: %d.\r\n"
  "Mensaje de error: %s.\r\n"
;

// Estructura que representa el contexto correspondiente al upload
// de los mensajes de correo. Tiene 3 campos; el número de bytes ya leidos,
// el texto que se va a enviar, y el tamaño del texto.
struct upload_status {
  size_t bytes_read;
  char * texto;
  int tam;
};

/**
 * Función que se utiliza con curl para leer los datos del string que se
 * quiere enviar en el correo electrónico.
 * @param ptr Donde se guarda la información leida.
 * @param size tamaño en bytes que queremos leer.
 * @param nmemb //FIXME
 * @param datos aqui se recibe un contexto correspondiente al upload.
 * @return retorna la cantidad de bytes leidos.
 */
static size_t payload_source(void * ptr, size_t size, size_t nmemb, void * datos) {
  struct upload_status * upload_ctx = (struct upload_status *)datos;
  const char * data;

  // Si el size, nmemb son cero o si la cantidad de bytes leidos es mayor al tamaño del texto retornamos cero.
  if (size == 0 || nmemb == 0 || upload_ctx->bytes_read >= (size_t)upload_ctx->tam) {
    return 0;
  }

  // Los datos que vamos a pasar a ptr serán los que estén en el texto a partir de los bytes que ya leimos.
  data = upload_ctx->texto + upload_ctx->bytes_read;

  // La cantidad de bytes a copiar es el menor entre lo que nos piden leer y lo que queda por leer.
  size_t len = min(size, (size_t)upload_ctx->tam - upload_ctx->bytes_read);
  memcpy(ptr, data, len);
  upload_ctx->bytes_read += len;

  // Retornamos la cantidad de bytes leidos.
  return len;
}

/**
 * Se encarga de eliminar un fin de linea de un string.
 * @param string string al que queremos quitarle el fin de linea.
 * @return retorna el string sin el fin de linea.
 */
char * chomp(char * string) {
  char * c = strchr(string, '\n');
  if (NULL != c) {
    *c = '\0';
  }
  return string;
}

/**
 * Se encarga de escribir en la bitácora un evento.
 * Lo escribe con el siguiente formato:
 * <serial> : <fecha> : <origen> : <código_de_evento> <mensaje_de_evento>
 * @param archivo archivo bitácora al que escribiremos.
 * @param evento evento que vamos a guardar en la bitácora.
 */
void escribir_bitacora(FILE * archivo, struct evento evento) {
  char buf[26];
  fprintf(archivo, "%d : ", evento.serial);
  char * str = chomp(ctime_r((time_t *)(&evento.fecha), buf));
  fprintf(archivo, "%s : ", str);
  fprintf(archivo, "%d : ", evento.origen);
  fprintf(archivo, "%d : ", evento.tipo);
  fprintf(archivo, "%s\n", to_s_te(evento.tipo));
  fflush(archivo);
}

/**
 * Se encarga de enviar una alerta por correo con respecto a un evento dado.
 * Se utiliza curl para facilitar el envio del correo.
 * @param evento evento que vamos a reportar por correo.
 */
void send_mail(struct evento evento) {
  CURL *curl;
  CURLcode res = CURLE_OK;
  struct curl_slist *recipients = NULL;
  struct upload_status upload_ctx;

  upload_ctx.bytes_read = 0;
  upload_ctx.tam = asprintf(&upload_ctx.texto, payload_text, to, evento.origen, evento.tipo, to_s_te(evento.tipo));
  if (-1 == upload_ctx.tam) {
    perror("asprintf");
    exit(EX_OSERR);
  }

  // Inicializamos el contexto para usar curl.
  curl = curl_easy_init();
  // Si lo hicimos exitosamente inicializamos los campos de opciones y enviamos el correo.
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, "smtp://smtp.ldc.usb.ve"); // Url del servidor smtp a utilizar.
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, FROM); // Dirección de donde se envia el correo.
    recipients = curl_slist_append(recipients, to); // Creamos una lista de destinatarios.
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients); // Indicamos que esta es la lista de destinatarios.
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source); // Indicamos la funcion de lectura.
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx); // Indicamos los datos del contexto que vamos a enviar.
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    // Enviamos el correo.
    res = curl_easy_perform(curl);

    // Si hubo algún error imprimimos un mensaje de error.
    if(res != CURLE_OK) {
      fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

    // Liberamos la lista de destinatarios.
    curl_slist_free_all(recipients);
    //Hacemos cleanup y aquí es que se hace quit.
    curl_easy_cleanup(curl);
  }
  free(upload_ctx.texto);
}

/**
 * Se encarga de encolar un file descriptor en la cola clientes
 * para que un consumidor se encargue de atender las solicitudes.
 * @param num file descriptor del socket listo para atender.
 */
void encolar(int num) {
  int s = pthread_mutex_lock(&mutex_clientes);
  if (s != 0) {
    // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
    errno = s;
    perror("Error intentando entrar en la sección crítica del productor; pthread_mutex_lock");
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
    perror("Error intentando salir de la sección crítica del productor; pthread_mutex_unlock");
    exit(EX_SOFTWARE);
  }
}

// Cola para llevar la cuenta de las ultimas conexiones de cada cliente.
Deque ultimas_conexiones;

struct ultima_conexion {
  uint32_t origen;
  uint64_t fecha;
};

/**
 * Se encarga de comparar dos conexiones para ver si provienen del mismo ATM.
 * @param actual_ conexión actual de la cola de conexiones.
 * @param origen_ conexión con la que se compara.
 * @return retorna 1 si son iguales y 0 en caso contrario.
 */
int comparar_conexion(void * actual_, void * origen_) {
  struct ultima_conexion * actual = actual_;
  uint32_t * origen = origen_;
  return actual->origen == *origen;
}

/**
 * Se encarga de detectar fallos de conectividad en los ATM, si la ultima
 * conexión es mayor a 5 minutos escribe en la bitacora un evento.
 * @param ultima_conexion_ ultima conexión de un ATM.
 */
void ver_ultima_conexion(void * ultima_conexion_) {
  struct ultima_conexion * ultima_conexion = (struct ultima_conexion *)ultima_conexion_;
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);

  int segundos = t.tv_sec - ultima_conexion->fecha;
  if (segundos > 5) { //FIXME Deberían ser 5 minutos pero para efectos de rapidez al probar usamos 5 segundos.
    printf("Timeout por retardo o perdida de conexion en el ATM: %u\n", ultima_conexion->origen);

    // Creamos un evento a partir de la información tomada anteriormente.
    struct evento evento =
      { .origen = ultima_conexion->origen
      , .fecha  = t.tv_sec
      , .tipo   = TE_FALLA_DE_CONEXION
      , .serial = numero_de_desconexiones++
      }
    ;

    pthread_mutex_lock(&mutex_stdout);
    { // Sección crítica
      escribir_bitacora(bitacora_file, evento);
      fflush(bitacora_file);
    }
    pthread_mutex_unlock(&mutex_stdout);

    // Eliminamos de la cola de conexiones a la conexión que acabamos de detectar como caida.
    delete_first_deque(comparar_conexion, ultimas_conexiones, &ultima_conexion->origen);
  }
}

/**
 * Se encarga de aceptar la conexion de algun cliente y encolarla en la cola clientes
 * para que algún consumidor atienda la solicitud. Es el productor.
 * @param socks cantidad de file descriptors de sockets.
 * @param socksfd arreglo de file descriptors de sockets.
 */
void aceptar_conexion(int socks, int sockfds[]) {
  fd_set readfds;
  int nfds = -1;

  FD_ZERO(&readfds);
  for (int i = 0; i < socks; ++i) {
    FD_SET(sockfds[i], &readfds);
    nfds = (sockfds[i] > nfds) ? sockfds[i] : nfds;
  }

  // Buscamos con select los file descriptors disponibles para leer de ellos.
  int disponibles;
  struct timeval t;
  //FIXME Deberían ser 30 segundos de timeout para revisar si hay conexiones caidas,
  // se utilizan 3 segundos para facilitar y acortar el tiempo de pruebas.
  t.tv_sec = 3;
  switch (disponibles = select(nfds + 1, &readfds, NULL, NULL, &t)) {
    case 0:
      break;

    case -1:
      perror("Error esperando por conexiones de clientes");
      exit(EX_IOERR);

    default:
      break;
  }

  // Verificamos para todas las conexiones en la cola de conexiones si alguna ha pasado
  // mas del tiempo determinado sin comunicarse con el servidor para detectar fallas de
  // conectividad.
  mapM_deque(ver_ultima_conexion, ultimas_conexiones);

  // Revisamos los sockets disponibles y si pertenecen al set retornado por
  // select entonces los encolamos para que un consumidor se encargue de atender
  // su solicitud.
  int j = 0;
  for (int i = 0;j < disponibles && i < socks; ++i) {
    if (FD_ISSET(sockfds[i], &readfds)) {
      ++j;

      encolar(sockfds[i]);
    }
  }
}

/**
 * Se encarga de desencolar una solicitud de la cola de clientes.
 * @param datos no esta siendo utilizado.
 * @return retorna lo que se saca de la cola.
 */
void * desencolar (void * datos) {
  (void)datos;

  return pop_front_deque(clientes);
}

/**
 * Se encarga de procesar una conexion de la cola de clientes.
 * @param f funcion con la que va a procesar la conexión de la cola de clientes.
 * @param datos
 * @return retorna lo que se saca de la cola.
 */
void * with_clientes(void * (*f)(void *), void * datos) {
  int s = pthread_mutex_lock(&mutex_clientes);
  if (s != 0) {
    // Si el código del programa está bien, esto nunca debería suceder.  Sin embargo, esta verificación puede ayudar a detectar errores de programación.
    errno = s;
    perror("Error intentando entrar en la sección crítica del consumidor; pthread_mutex_lock");
    exit(EX_SOFTWARE);
  }

  // Mientras NO haya datos en la pila…
  while (!(length_deque(clientes) > 0)) {
    // …luego, si no nos salimos, esperamos a que alguien inserte datos.
    // Cuando se entra a esta función, atómicamente se libera el mutex y se
    // comienza a esperar por un signal sobre la condición.
    s = pthread_cond_wait(&cond_stack_readable, &mutex_clientes);
    if (s != 0) {
      // Si el código del programa está bien, esto nunca debería suceder.
      // Sin embargo, esta verificación puede ayudar a detectar errores de programación.
      errno = s;
      perror("Error intentando entrar en la sección crítica del consumidor; pthread_mutex_lock");
      exit(EX_SOFTWARE);
    }
    // Al ocurrir un signal sobre esta condición, esta función adquiere de
    // nuevo el mutex y retorna.  Si otro consumidor no se nos adelantó, la
    // condición del ciclo no se cumplirá (porque seremos los primeros en ver
    // el nuevo dato disponible en la pila) y saldremos del ciclo.
  }

  void * ret = f(datos);

  s = pthread_mutex_unlock(&mutex_clientes);
  if (s != 0) {
    // Si el código del programa está bien, esto nunca debería suceder.
    // Sin embargo, esta verificación puede ayudar a detectar errores de programación.
    errno = s;
    perror("Error intentando salir de la sección crítica del consumidor; pthread_mutex_unlock");
    exit(EX_SOFTWARE);
  }

  return ret;
}

/**
 * Se encarga de verificar si el arreglo de patrones que debemos detectar
 * para enviar correos con alertas contiene el codigo indicado.
 * @param codigo codigo que vamos a chequear si existe en el arreglo.
 * @return retorna 1 si pertenece y 0 en caso contrario.
 * funciones de hilos.
 */
int patrones_contains(int codigo) {
  size_t i = 0;

  while((i < sizeof(N_PATRONES)) && (0 != patrones[i])) {
    if (codigo == patrones[i]) {
      return 1;
    }
    ++i;
  }
  return 0;
}

/**
 * Se encarga de consumir una conexion de la cola de clientes.
 * @param arg número de consumidor atendiendo al cliente.
 * @return retorno no utilizado, solo existe para adaptarse a la firma necesaria para
 * funciones de hilos.
 */
void * consumidor(void * arg) {
  int * num_consumidor_p = (int *)arg;
  int num_consumidor = *num_consumidor_p;
  free(num_consumidor_p);

  // Aceptamos la conexión de algun cliente.
  while(1) {
    int * listener_p = (int *)with_clientes(desencolar, NULL);
    int listener = *listener_p;
    free(listener_p);

    int cliente = accept(listener, NULL, NULL);
    if (-1 == cliente) {
      if (EAGAIN == errno || EWOULDBLOCK == errno) {
        continue;
      } else {
        perror("Error aceptando la conexión del cliente");
        exit(EX_IOERR);
      }
    }

    // Recibimos un evento del cliente
    struct evento evento = recibir(cliente);

    // Si no es valido cerramos la conexion y continuamos ejecutando.
    if (!evento_valido(evento)) {
      close(cliente);
      continue;
    }

    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);

    struct ultima_conexion * ultima_conexion;

    // Si es un evento válido actualizamos la cola de ultimas conexiones para este ATM.
    // Si ya existe en la cola actualizamos su ultima conexión y si no existe lo agregamos.
    if (NULL != (ultima_conexion = (struct ultima_conexion *)find_deque(comparar_conexion, ultimas_conexiones, &evento.origen))) {
      ultima_conexion->fecha = t.tv_sec;
    } else {
      struct ultima_conexion * nueva_conexion = calloc(1, sizeof(struct ultima_conexion));
      nueva_conexion->origen = evento.origen;
      nueva_conexion->fecha  = t.tv_sec;

      push_front_deque(ultimas_conexiones, nueva_conexion);
    }

    // Escribimos en la bitácora el evento.
    pthread_mutex_lock(&mutex_stdout);
    { // Sección crítica
      printf("Consumidor %d: recibí: %s.\n", num_consumidor, to_s_te(evento.tipo));
      fflush(stdout);
      if (TE_HEARTBEAT != evento.tipo) {
        escribir_bitacora(bitacora_file, evento);
        fflush(bitacora_file);
      }
    }
    pthread_mutex_unlock(&mutex_stdout);

    // Si el evento es alguno contenido en el arreglo de patrones a detectar,
    // enviamos un correo alerta.
    if (patrones_contains(evento.tipo)) {
      send_mail(evento);
    }

    // Cerramos la conexión.
    close(cliente);
  }
}

/**
 * Se encarga de leer el archivo de configuración que indica el correo
 * al que se van a enviar las alertas y los códigos de eventos que debemos
 * detectar para alertar.
 */
void leer_config(void) {
  //Abrimos el archivo de configuración
  config_file = fopen(config, "r");
  if (NULL == config_file) {
    fprintf(stderr, "fopen: %s: ", config);
    perror("");
    exit(EX_IOERR);
  }

  // Leemos el correo.
  char * correo;
  if (1 == fscanf(config_file, " %ms", &correo)) {
    to = correo;
  }

  // Leemos los códigos de eventos.
  int num, i = 0;
  while (1 == fscanf(config_file, "%d", &num)) {
    patrones[i] = num;
    ++i;
  }

  //Cerramos el archivo de configuración
  fclose(config_file);
}

/**
 * Es el main del programa se encarga de parsear los argumentos de linea de comandos,
 * configurar el socket del servidor, de crear a los hilos consumidores que se encargan
 * de procesar las solicitudes y conexiones de los clientes y de aceptar las conexiones
 * de los clientes.
 */
int main(int argc, char ** argv) {
  char opt;

  { // Inicializamos el arreglo de patrones.
    size_t i = 0;
    while (i < sizeof(N_PATRONES)) {
      patrones[i] = 0;
      ++i;
    }
  }

  // Asignamos el nombre del programa.
  program_name = argv[0];

  // Parseamos los argumentos -l, -b y -c de la linea de comandos
  // Si se pone alguna opción que no sea una de las anteriores emitimos un error
  // para indicarle la correcta invocación al usuario.
  while ((opt = getopt(argc, argv, "l:b:c:")) != -1) {
    switch (opt) {
      case 'l': puerto   = optarg; break;
      case 'b': bitacora = optarg; break;
      case 'c': config   = optarg; break;
      default:
        exit_usage(EX_USAGE);
    }
  }

  // Si no se indicó el puerto se emite un error.
  if (NULL == puerto) {
    fprintf(stderr, "El número de puerto local es obligatorio.\n");
    exit_usage(EX_USAGE);
  }

  // Si no se indicó el archivo bitacora se emite un error.
  if (NULL == bitacora) {
    fprintf(stderr, "El nombre del archivo bitácora es obligatorio.\n");
    exit_usage(EX_USAGE);
  }

  // Si se indicó el archivo de configuración leemos su configuración.
  if (NULL != config) {
    leer_config();
  }

  // Llenamos los hints necesarios.
  struct addrinfo hints;
  struct addrinfo * results;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_addr      = NULL;
  hints.ai_canonname = NULL;
  hints.ai_family    = AF_INET;                        // Direcciones IPv4.
  hints.ai_flags     = AI_PASSIVE || AI_NUMERICSERV;   // Socket que pueda hacer accept y que use puertos numéricos.
  hints.ai_next      = NULL;
  hints.ai_protocol  = getprotobyname("TCP")->p_proto; // Protocolo TCP.
  hints.ai_socktype  = SOCK_STREAM;                    // Socket basado en conexiones.

  { // Buscamos las direcciones a las que podemos hacer bind.
    int i;
    if ((i = getaddrinfo(NULL, puerto, &hints, &results)) != 0) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(i));
      exit(EX_OSERR);
    }
  }

  // Verificamos cuantas direcciones retornó getaddrinfo.
  int num_addrs = 0;
  for (struct addrinfo * result = results; result != NULL; result = result->ai_next) ++num_addrs;

  // Si no retorna alguna no hay manera de crear el servicio.
  if (0 == num_addrs) {
    fprintf(stderr, "No se encontró ninguna manera de crear el servicio.\n");
    exit(EX_UNAVAILABLE);
  }

  int * sockfds;
  if ((sockfds = (int *)calloc(num_addrs, sizeof(int))) == NULL) {
    perror("calloc");
    exit(EX_OSERR);
  }

  // Revisamos las direcciones que retorna getaddrinfo para ver cual podemos utilizar.
  int i = 0;
  int socks = 0;
  for (struct addrinfo * result = results; result != NULL; ++i, result = result->ai_next) {
    // Si no podemos crear un socket entonces le asignamos a esa dirección el valor de no_sock y continuamos revisando.
    if ((sockfds[i] = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1) {
      sockfds[i] = no_sock;
      continue;
    }

    // Intentamos hacer bind. Si falla, cerramos, asignamos no_sock y continuamos revisando,
    // sino, se logra "bindear" exitosamente
    if (bind(sockfds[i], result->ai_addr, result->ai_addrlen) == -1) {
      close(sockfds[i]);
      sockfds[i] = no_sock;
      continue;
    }

    // Intentamos escuchar por este socket. Si falla, cerramos la conexion, asignamos no_sock y continuamos revisando.
    if (listen(sockfds[i], default_backlog) == -1) {
      close(sockfds[i]);
      sockfds[i] = no_sock;
      continue;
    }

    ++socks;
  }

  // Liberamos results.
  freeaddrinfo(results);

  // Si no logramos crear algun socket salimos del programa con error.
  if (socks <= 0) {
    fprintf(stderr, "No se encontró ninguna manera de crear el servicio.\n");
    exit(EX_UNAVAILABLE);
  }

  // Pasamos todos los sockets que si pudimos crear exitosamente al inicio del arreglo de sockets.
  i = 0;
  int j = 0;
  for (i = 0, j = 0; i < socks; ++i) if (sockfds[i] == no_sock) {
    if (j == 0) j = i+1;
    for (; j < num_addrs; ++j) if (sockfds[j] != no_sock) break;
    sockfds[i] = sockfds[j];
    ++j;
  }

  // Hacemos realloc de la memoria para volverla mas pequeña.
  if ((sockfds = (int *)realloc(sockfds, socks*sizeof(int))) == NULL) {
    perror("realloc");
    exit(EX_OSERR);
  }

  // Hacemos que los sockets sean non-blocking.
  for (i = 0; i < socks; ++i) if (fcntl(sockfds[i], F_SETFL, O_NONBLOCK) == -1) {
    perror("fcntl");
    exit(EX_OSERR);
  }

  // Inicializamos curl.
  if (0 != curl_global_init(CURL_GLOBAL_ALL)) {
    fprintf(stderr, "No se pudo inicializar cURL\n");
    exit(EX_UNAVAILABLE);
  }

  int num_consumidores = 10;
  clientes = empty_deque();
  ultimas_conexiones = empty_deque();
  pthread_t consumidores[num_consumidores];
  int s;

  // Creamos los consumidores que se encargaran de atender las solicitudes de
  // los clientes.
  for (int i = 0; i < num_consumidores; ++i) {
    int * num_consumidor = malloc(sizeof(int));
    *num_consumidor = i;
    s = pthread_create(&consumidores[i], NULL, &consumidor, num_consumidor);
    if (s != 0) {
      errno = s;
      perror("No fue posible crear hilo consumidor; pthread_create: ");
      exit(EX_OSERR);
    }
  }

  //Abrimos la bitacora
  bitacora_file = fopen(bitacora, "w");
  if (NULL == bitacora_file) {
    fprintf(stderr, "fopen: %s: ", bitacora);
    perror("");
    exit(EX_IOERR);
  }

  // Aceptamos conexiones.
  while (1) {
    aceptar_conexion(socks, sockfds);
  }

  //Cerramos la bitacora
  fclose(bitacora_file);
}
