#include <stdio.h> // entrada y salida estandar, printf()
#include <stdlib.h> // utilidades generales de C, malloc(), free(), exit()
#include <string.h> // funciones para manipular cadenas
#include <unistd.h> // read(), STDIN_FILENO
#include <sys/select.h> // select(), para esperar teclado con tiempo limite
#include <pthread.h> // mutex y variable de condicion para esperar eventos de msquic
#include <msquic.h> // libreria msquic: implementacion de QUIC (transporte confiable sobre UDP + TLS 1.3)
#define PUERTO 7000 // puerto UDP del broker QUIC
#define MAX_MENSAJE 256 // maximo tamanio de un mensaje (sin contar el '\n')
#define MAX_PARTIDOS 8 // maximo de partidos que se pueden seguir
#define MAX_NOMBRE 50 // maximo tamanio del nombre de un partido

//ALPN: nombre del protocolo de aplicacion negociado en el handshake TLS (debe coincidir con el broker)
const QUIC_BUFFER alpn = { sizeof("pubsub") - 1, (uint8_t*)"pubsub" };

//tabla de funciones de msquic, la llena MsQuicOpen2
const QUIC_API_TABLE* msquic;

//estado de la conexion; los callbacks de msquic corren en otros hilos y el hilo principal
//espera los cambios con una variable de condicion protegida por un mutex
pthread_mutex_t mutex_estado = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cambio_estado = PTHREAD_COND_INITIALIZER;
int conectado = 0; // 1 cuando termina el handshake
int conexion_terminada = 0; // 1 cuando la conexion se cerro por completo

//bytes recibidos que todavia no forman un mensaje completo (un stream es un flujo de bytes,
//igual que TCP, asi que un mensaje puede llegar partido o pegado con otro)
char buffer_recepcion[MAX_MENSAJE + 1];
int bytes_buffer = 0;
int mensajes_recibidos = 0;


//marca un cambio de estado y despierta al hilo principal
void notificar_estado(int* variable)
{
    pthread_mutex_lock(&mutex_estado);
    *variable = 1;
    pthread_cond_broadcast(&cambio_estado);
    pthread_mutex_unlock(&mutex_estado);
}

//envia un mensaje por el stream agregandole el '\n' que lo delimita
int enviar_mensaje(HQUIC stream, const char* mensaje)
{
    char linea[MAX_MENSAJE + 2];
    snprintf(linea, sizeof(linea), "%s\n", mensaje);
    int longitud = strlen(linea);

    //StreamSend es asincrono: el buffer debe seguir existiendo hasta el evento SEND_COMPLETE,
    //por eso se reserva con malloc (estructura QUIC_BUFFER + bytes del mensaje en un solo bloque)
    QUIC_BUFFER* buffer_envio = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) + longitud);
    if (buffer_envio == NULL)
        return -1;
    buffer_envio->Buffer = (uint8_t*)(buffer_envio + 1);
    buffer_envio->Length = longitud;
    memcpy(buffer_envio->Buffer, linea, longitud);

    //StreamSend encola datos para enviarlos por el stream (equivale a send()/write() en TCP)
    //recibe: el stream, el arreglo de buffers y su cantidad (1), flags (NONE) y un contexto
    //que msquic devuelve en SEND_COMPLETE (el mismo buffer, para liberarlo ahi)
    if (QUIC_FAILED(msquic->StreamSend(stream, buffer_envio, 1, QUIC_SEND_FLAG_NONE, buffer_envio))) {
        free(buffer_envio);
        return -1;
    }
    return 0;
}

//muestra en pantalla un mensaje completo recibido del broker
void mostrar_mensaje(char* linea)
{
    //confirmacion del registro: "OK|partido1,partido2"
    if (strncmp(linea, "OK|", 3) == 0) {
        printf("Suscripcion confirmada por el broker: %s\n", linea + 3);
        return;
    }
    //noticia: "partido|texto"
    char* separador = strchr(linea, '|');
    mensajes_recibidos++;
    if (separador == NULL) {
        printf("(%d) %s\n", mensajes_recibidos, linea);
        return;
    }
    *separador = '\0';
    printf("(%d) [%s] %s\n", mensajes_recibidos, linea, separador + 1);
}


//callback del stream: eventos del stream con el broker
QUIC_STATUS QUIC_API callback_stream(HQUIC stream, void* contexto, QUIC_STREAM_EVENT* evento)
{
    (void)contexto; // no se usa

    switch (evento->Type) {

    //llegaron datos del broker (equivale a que recv() devuelva bytes en TCP)
    //se acumulan en buffer_recepcion y se corta un mensaje en cada '\n'
    case QUIC_STREAM_EVENT_RECEIVE:
        for (uint32_t i = 0; i < evento->RECEIVE.BufferCount; i++) {
            const QUIC_BUFFER* recibido = &evento->RECEIVE.Buffers[i];
            for (uint32_t k = 0; k < recibido->Length; k++) {
                char c = (char)recibido->Buffer[k];
                if (c == '\n') {
                    buffer_recepcion[bytes_buffer] = '\0';
                    if (bytes_buffer > 0)
                        mostrar_mensaje(buffer_recepcion);
                    bytes_buffer = 0;
                }
                else if (bytes_buffer < MAX_MENSAJE) {
                    buffer_recepcion[bytes_buffer++] = c;
                }
            }
        }
        fflush(stdout);
        break;

    //msquic termino de enviar (o cancelo) un StreamSend: se libera el buffer
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        free(evento->SEND_COMPLETE.ClientContext);
        break;

    //el broker cerro su direccion de envio: cerramos la nuestra tambien
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        break;

    //ambas direcciones del stream terminaron: se libera el handle con StreamClose
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        msquic->StreamClose(stream);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//callback de la conexion: eventos de la conexion con el broker
QUIC_STATUS QUIC_API callback_conexion(HQUIC conexion, void* contexto, QUIC_CONNECTION_EVENT* evento)
{
    (void)conexion; (void)contexto; // no se usan

    switch (evento->Type) {

    //termino el handshake QUIC + TLS 1.3 (1 RTT)
    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("Conectado al broker correctamente..\n");
        notificar_estado(&conectado);
        break;

    //la conexion se cerro por el transporte: broker inalcanzable, caido (timeout), etc.
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //en Linux los QUIC_STATUS son codigos errno, strerror los traduce a texto
        printf("Conexion cerrada por el transporte (%s): el broker no responde o rechazo la conexion\n",
               strerror(evento->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
        break;

    //el broker cerro la conexion (por ejemplo, se detuvo con Enter)
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("El broker cerro la conexion\n");
        break;

    //la conexion termino por completo (el handle lo cierra el hilo principal)
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        notificar_estado(&conexion_terminada);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}


int main(int argc, char* argv[])
{
    QUIC_STATUS estado; // codigo de resultado de las funciones de msquic
    HQUIC registro; // contexto de ejecucion de la aplicacion dentro de msquic
    HQUIC configuracion; // parametros de la conexion (ALPN, tiempos, credenciales)
    HQUIC conexion; // conexion QUIC con el broker (equivale al socket del cliente en TCP)
    HQUIC stream; // stream bidireccional por donde llegan las noticias
    char mensaje[MAX_MENSAJE + 1];

    //uso: ./subscriber_quic <ip_broker> <partido1> [partido2 ...]
    if (argc < 3) {
        printf("Uso: %s <ip_broker> <partido1> [partido2 ...]\n", argv[0]);
        printf("Ejemplo: %s 192.168.1.20 ColombiaVsBrasil RealVsBarca\n", argv[0]);
        exit(0);
    }
    const char* ip_broker = argv[1];
    if (argc - 2 > MAX_PARTIDOS) {
        printf("Se pueden seguir maximo %d partidos\n", MAX_PARTIDOS);
        exit(0);
    }

    //mensaje de registro: "SUBSCRIBER|partido1,partido2,..."
    snprintf(mensaje, sizeof(mensaje), "SUBSCRIBER|");
    for (int i = 2; i < argc; i++) {
        if (strpbrk(argv[i], "|,") != NULL || strlen(argv[i]) >= MAX_NOMBRE) {
            printf("Nombre de partido invalido: %s (sin '|' ni ',', maximo %d caracteres)\n", argv[i], MAX_NOMBRE - 1);
            exit(0);
        }
        if (i > 2) strncat(mensaje, ",", sizeof(mensaje) - strlen(mensaje) - 1);
        strncat(mensaje, argv[i], sizeof(mensaje) - strlen(mensaje) - 1);
    }

    //MsQuicOpen2 carga la libreria y llena la tabla de funciones de la API (version 2)
    if (QUIC_FAILED(estado = MsQuicOpen2(&msquic))) {
        printf("Error al abrir msquic (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //RegistrationOpen crea el contexto de ejecucion (nombre de la app + perfil de baja latencia)
    const QUIC_REGISTRATION_CONFIG config_registro = { "subscriber_quic", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(estado = msquic->RegistrationOpen(&config_registro, &registro))) {
        printf("Error al crear el registro (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //parametros de la conexion (solo se aplican los campos marcados con IsSet)
    QUIC_SETTINGS parametros;
    memset(&parametros, 0, sizeof(parametros));
    //IdleTimeoutMs: si en 30 s no llega nada del broker, la conexion se da por muerta
    //(asi el suscriptor detecta que el broker se cayo)
    parametros.IdleTimeoutMs = 30000;
    parametros.IsSet.IdleTimeoutMs = TRUE;
    //KeepAliveIntervalMs: cada 5 s se envia un PING; el suscriptor puede pasar mucho tiempo
    //sin recibir noticias y sin esto la conexion se cerraria por inactividad
    parametros.KeepAliveIntervalMs = 5000;
    parametros.IsSet.KeepAliveIntervalMs = TRUE;

    //ConfigurationOpen crea la configuracion: registro, lista de ALPN y cantidad,
    //parametros y su tamanio, contexto (NULL) y puntero donde dejar el handle
    if (QUIC_FAILED(estado = msquic->ConfigurationOpen(registro, &alpn, 1, &parametros, sizeof(parametros), NULL, &configuracion))) {
        printf("Error al crear la configuracion (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //credenciales del cliente: sin certificado propio (NONE), flag CLIENT y sin validar el
    //certificado del broker (NO_CERTIFICATE_VALIDATION) porque es autofirmado
    QUIC_CREDENTIAL_CONFIG credenciales;
    memset(&credenciales, 0, sizeof(credenciales));
    credenciales.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credenciales.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    //ConfigurationLoadCredential carga las credenciales TLS en la configuracion
    if (QUIC_FAILED(estado = msquic->ConfigurationLoadCredential(configuracion, &credenciales))) {
        printf("Error al cargar las credenciales (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //ConnectionOpen crea el objeto de la conexion, todavia sin conectar (equivale a socket())
    //recibe el registro, la funcion callback, un contexto (NULL) y un puntero para el handle
    if (QUIC_FAILED(estado = msquic->ConnectionOpen(registro, callback_conexion, NULL, &conexion))) {
        printf("Error al crear la conexion (0x%x)\n", (unsigned int)estado);
        exit(0);
    }
    else
        printf("Conexion QUIC creada correctamente..\n");

    //ConnectionStart inicia el handshake QUIC + TLS 1.3 con el broker (equivale a connect())
    //recibe la conexion, la configuracion, la familia de direcciones (UNSPEC = IPv4 o IPv6
    //segun la IP), la IP del broker como texto y el puerto
    //es asincrono: el resultado llega en el evento CONNECTED
    if (QUIC_FAILED(estado = msquic->ConnectionStart(conexion, configuracion, QUIC_ADDRESS_FAMILY_UNSPEC, ip_broker, PUERTO))) {
        printf("Error al iniciar la conexion con %s:%d (0x%x)\n", ip_broker, PUERTO, (unsigned int)estado);
        exit(0);
    }
    printf("Conectando con el broker %s:%d..\n", ip_broker, PUERTO);

    //se espera a que termine el handshake o a que la conexion falle
    pthread_mutex_lock(&mutex_estado);
    while (!conectado && !conexion_terminada)
        pthread_cond_wait(&cambio_estado, &mutex_estado);
    pthread_mutex_unlock(&mutex_estado);
    if (!conectado) {
        printf("No se pudo conectar con el broker\n");
        msquic->ConnectionClose(conexion);
        exit(0);
    }

    //StreamOpen crea un stream bidireccional dentro de la conexion
    //recibe la conexion, flags (NONE = bidireccional), la funcion callback del stream,
    //un contexto (NULL) y un puntero donde dejar el handle
    if (QUIC_FAILED(estado = msquic->StreamOpen(conexion, QUIC_STREAM_OPEN_FLAG_NONE, callback_stream, NULL, &stream))) {
        printf("Error al crear el stream (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //StreamStart activa el stream (le asigna su identificador dentro de la conexion)
    if (QUIC_FAILED(estado = msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("Error al iniciar el stream (0x%x)\n", (unsigned int)estado);
        msquic->StreamClose(stream);
        exit(0);
    }

    //se envia el registro; desde aqui el broker reenviara por este mismo stream
    //las noticias de los partidos suscritos
    enviar_mensaje(stream, mensaje);
    printf("Esperando noticias (presione Enter para salir)..\n");

    //el hilo principal espera a que el usuario presione Enter o a que la conexion se cierre
    //las noticias se imprimen desde el callback del stream (en un hilo de msquic)
    //select() espera el teclado como maximo 1 s, para revisar periodicamente si la conexion termino
    while (!conexion_terminada) {
        fd_set descriptores_lectura;
        struct timeval espera = { 1, 0 }; // 1 segundo
        FD_ZERO(&descriptores_lectura);
        FD_SET(STDIN_FILENO, &descriptores_lectura);
        if (select(STDIN_FILENO + 1, &descriptores_lectura, NULL, NULL, &espera) > 0) {
            char tecla[64];
            if (read(STDIN_FILENO, tecla, sizeof(tecla)) >= 0) // se presiono Enter (o se cerro la entrada)
                break;
        }
    }

    //ConnectionShutdown cierra la conexion (envia CONNECTION_CLOSE al broker)
    //recibe la conexion, flags (NONE = cierre ordenado) y un codigo de error de aplicacion (0)
    if (!conexion_terminada)
        msquic->ConnectionShutdown(conexion, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    pthread_mutex_lock(&mutex_estado);
    while (!conexion_terminada)
        pthread_cond_wait(&cambio_estado, &mutex_estado);
    pthread_mutex_unlock(&mutex_estado);

    //se liberan los handles en orden inverso a su creacion
    msquic->ConnectionClose(conexion);
    msquic->ConfigurationClose(configuracion);
    msquic->RegistrationClose(registro);
    MsQuicClose(msquic);
    printf("Suscriptor finalizado (%d noticias recibidas)\n", mensajes_recibidos);
    return 0;
}
