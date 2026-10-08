#include <stdio.h> // entrada y salida estandar, printf(), fgets()
#include <stdlib.h> // utilidades generales de C, malloc(), free(), exit()
#include <string.h> // funciones para manipular cadenas
#include <unistd.h> // sleep()
#include <time.h> // clock_gettime(), para esperas con tiempo limite
#include <pthread.h> // mutex y variable de condicion para esperar eventos de msquic
#include <msquic.h> // libreria msquic: implementacion de QUIC (transporte confiable sobre UDP + TLS 1.3)
#define PUERTO 7000 // puerto UDP del broker QUIC
#define MAX_MENSAJE 256 // maximo tamanio de un mensaje (sin contar el '\n')
#define NUM_EVENTOS 10 // eventos que envia el modo automatico

//ALPN: nombre del protocolo de aplicacion negociado en el handshake TLS (debe coincidir con el broker)
const QUIC_BUFFER alpn = { sizeof("pubsub") - 1, (uint8_t*)"pubsub" };

//tabla de funciones de msquic, la llena MsQuicOpen2
const QUIC_API_TABLE* msquic;

//estado de la conexion. Los callbacks de msquic corren en otros hilos, asi que el hilo
//principal espera los cambios con una variable de condicion protegida por un mutex
pthread_mutex_t mutex_estado = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cambio_estado = PTHREAD_COND_INITIALIZER;
int conectado = 0; // 1 cuando termina el handshake
int conexion_terminada = 0; // 1 cuando la conexion se cerro por completo
int stream_terminado = 0; // 1 cuando el stream se cerro por completo

//eventos de ejemplo del modo automatico
const char* eventos[NUM_EVENTOS] = {
    "Inicio del partido",
    "Tarjeta amarilla al numero 10 de Equipo B al minuto 12",
    "Gol de Equipo A al minuto 32",
    "Cambio: jugador 10 entra por jugador 20 en Equipo B",
    "Fin del primer tiempo: Equipo A 1 - 0 Equipo B",
    "Gol de Equipo B al minuto 58",
    "Tarjeta roja al numero 4 de Equipo A al minuto 67",
    "Gol de Equipo A al minuto 81",
    "Cambio: jugador 7 entra por jugador 11 en Equipo A",
    "Final del partido: Equipo A 2 - 1 Equipo B"
};


//marca un cambio de estado y despierta al hilo principal
void notificar_estado(int* variable)
{
    pthread_mutex_lock(&mutex_estado);
    *variable = 1;
    pthread_cond_broadcast(&cambio_estado);
    pthread_mutex_unlock(&mutex_estado);
}

//espera hasta que la variable sea 1 o pasen 'segundos' (0 = sin limite)
void esperar_estado(int* variable, int segundos)
{
    struct timespec limite;
    clock_gettime(CLOCK_REALTIME, &limite);
    limite.tv_sec += segundos;
    pthread_mutex_lock(&mutex_estado);
    while (!*variable) {
        if (segundos == 0)
            pthread_cond_wait(&cambio_estado, &mutex_estado);
        else if (pthread_cond_timedwait(&cambio_estado, &mutex_estado, &limite) != 0)
            break; // se acabo el tiempo
    }
    pthread_mutex_unlock(&mutex_estado);
}

//envia un mensaje por el stream agregandole el '\n' que lo delimita
//devuelve 0 si se pudo encolar y -1 si hubo error
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


//callback del stream: eventos del stream con el broker
QUIC_STATUS QUIC_API callback_stream(HQUIC stream, void* contexto, QUIC_STREAM_EVENT* evento)
{
    (void)contexto; // no se usa

    switch (evento->Type) {

    //el broker envio datos: solo manda la confirmacion "OK|partido\n"
    case QUIC_STREAM_EVENT_RECEIVE:
        for (uint32_t i = 0; i < evento->RECEIVE.BufferCount; i++) {
            //%.*s imprime exactamente Length bytes (el buffer no termina en '\0')
            printf("Broker: %.*s", (int)evento->RECEIVE.Buffers[i].Length, (char*)evento->RECEIVE.Buffers[i].Buffer);
        }
        fflush(stdout);
        break;

    //msquic termino de enviar (o cancelo) un StreamSend: se libera el buffer
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        free(evento->SEND_COMPLETE.ClientContext);
        break;

    //ambas direcciones del stream terminaron: se libera el handle con StreamClose
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        msquic->StreamClose(stream);
        notificar_estado(&stream_terminado);
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

    //termino el handshake QUIC + TLS 1.3 (1 RTT): ya se pueden abrir streams
    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("Conectado al broker correctamente..\n");
        notificar_estado(&conectado);
        break;

    //la conexion se cerro por el transporte: broker inalcanzable, timeout, etc.
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //en Linux los QUIC_STATUS son codigos errno, strerror los traduce a texto
        printf("Conexion cerrada por el transporte (%s): el broker no responde o rechazo la conexion\n",
               strerror(evento->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
        break;

    //el broker cerro la conexion
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
    HQUIC stream; // stream bidireccional por donde se envian los mensajes
    char mensaje[MAX_MENSAJE + 1];

    //uso: ./publisher_quic <ip_broker> <partido> [auto|manual]
    if (argc < 3) {
        printf("Uso: %s <ip_broker> <partido> [auto|manual]\n", argv[0]);
        printf("Ejemplo: %s 192.168.1.20 ColombiaVsBrasil auto\n", argv[0]);
        exit(0);
    }
    const char* ip_broker = argv[1];
    const char* partido = argv[2];
    int modo_manual = (argc >= 4 && strcmp(argv[3], "manual") == 0);

    //el nombre del partido no puede llevar los separadores del protocolo
    if (strpbrk(partido, "|,") != NULL) {
        printf("El nombre del partido no puede contener '|' ni ','\n");
        exit(0);
    }

    //MsQuicOpen2 carga la libreria y llena la tabla de funciones de la API (version 2)
    if (QUIC_FAILED(estado = MsQuicOpen2(&msquic))) {
        printf("Error al abrir msquic (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //RegistrationOpen crea el contexto de ejecucion (nombre de la app + perfil de baja latencia)
    const QUIC_REGISTRATION_CONFIG config_registro = { "publisher_quic", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(estado = msquic->RegistrationOpen(&config_registro, &registro))) {
        printf("Error al crear el registro (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //parametros de la conexion (solo se aplican los campos marcados con IsSet)
    QUIC_SETTINGS parametros;
    memset(&parametros, 0, sizeof(parametros));
    //IdleTimeoutMs: si en 30 s no llega nada del broker, la conexion se da por muerta
    parametros.IdleTimeoutMs = 30000;
    parametros.IsSet.IdleTimeoutMs = TRUE;
    //KeepAliveIntervalMs: cada 5 s se envia un paquete PING para mantener viva la conexion
    //(en modo manual el periodista puede tardar en escribir el siguiente evento)
    parametros.KeepAliveIntervalMs = 5000;
    parametros.IsSet.KeepAliveIntervalMs = TRUE;

    //ConfigurationOpen crea la configuracion: registro, lista de ALPN y cantidad,
    //parametros y su tamanio, contexto (NULL) y puntero donde dejar el handle
    if (QUIC_FAILED(estado = msquic->ConfigurationOpen(registro, &alpn, 1, &parametros, sizeof(parametros), NULL, &configuracion))) {
        printf("Error al crear la configuracion (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //credenciales del cliente:
    // Type NONE: el cliente no presenta certificado propio
    // flag CLIENT: indica que esta configuracion es de un cliente
    // flag NO_CERTIFICATE_VALIDATION: no verificar el certificado del broker, porque es
    //    autofirmado (en un sistema real se verificaria contra una autoridad certificadora)
    QUIC_CREDENTIAL_CONFIG credenciales;
    memset(&credenciales, 0, sizeof(credenciales));
    credenciales.Type = QUIC_CREDENTIAL_TYPE_NONE;
    credenciales.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    //ConfigurationLoadCredential carga las credenciales TLS en la configuracion
    if (QUIC_FAILED(estado = msquic->ConfigurationLoadCredential(configuracion, &credenciales))) {
        printf("Error al cargar las credenciales (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //ConnectionOpen crea el objeto de la conexion, todavia sin conectar
    //(equivale a socket() en TCP). Recibe el registro, la funcion callback de la conexion,
    //un contexto (NULL) y un puntero donde dejar el handle
    if (QUIC_FAILED(estado = msquic->ConnectionOpen(registro, callback_conexion, NULL, &conexion))) {
        printf("Error al crear la conexion (0x%x)\n", (unsigned int)estado);
        exit(0);
    }
    else
        printf("Conexion QUIC creada correctamente..\n");

    //ConnectionStart inicia el handshake QUIC + TLS 1.3 con el broker (equivale a connect() en TCP)
    //recibe cinco parametros:
    // la conexion
    // la configuracion a usar (ALPN, tiempos, credenciales)
    // la familia de direcciones: UNSPEC deja que msquic decida IPv4 o IPv6 segun la IP
    // la IP (o nombre) del broker como texto: msquic la convierte a binario (como inet_addr)
    // el puerto del broker
    //es asincrono: devuelve de inmediato y el resultado llega en el evento CONNECTED
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

    //StreamOpen crea un stream dentro de la conexion. QUIC puede llevar muchos streams
    //independientes en una misma conexion; usamos uno solo, bidireccional, para que los
    //eventos del partido lleguen en orden. Recibe la conexion, flags (NONE = bidireccional),
    //la funcion callback del stream, un contexto (NULL) y un puntero donde dejar el handle
    if (QUIC_FAILED(estado = msquic->StreamOpen(conexion, QUIC_STREAM_OPEN_FLAG_NONE, callback_stream, NULL, &stream))) {
        printf("Error al crear el stream (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //StreamStart activa el stream (le asigna su identificador dentro de la conexion)
    //recibe el stream y flags (NONE)
    if (QUIC_FAILED(estado = msquic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
        printf("Error al iniciar el stream (0x%x)\n", (unsigned int)estado);
        msquic->StreamClose(stream);
        exit(0);
    }

    //mensaje de registro: "PUBLISHER|partido"
    snprintf(mensaje, sizeof(mensaje), "PUBLISHER|%s", partido);
    enviar_mensaje(stream, mensaje);

    if (!modo_manual) {
        //modo automatico: 10 eventos, uno por segundo
        for (int i = 0; i < NUM_EVENTOS && !conexion_terminada; i++) {
            sleep(1);
            //formato de la noticia: "partido|[numero] texto"
            //el numero de evento permite verificar en el suscriptor que no falte ninguno y el orden
            snprintf(mensaje, sizeof(mensaje), "%s|[%02d] %s", partido, i + 1, eventos[i]);
            if (enviar_mensaje(stream, mensaje) == 0)
                printf("Enviado: %s\n", mensaje);
            else
                printf("Error al enviar: %s\n", mensaje);
        }
    }
    else {
        //modo manual: cada linea escrita es un evento, hasta escribir "exit"
        char texto[MAX_MENSAJE];
        printf("Escriba los eventos del partido %s (\"exit\" para terminar):\n", partido);
        while (!conexion_terminada && fgets(texto, sizeof(texto), stdin) != NULL) {
            texto[strcspn(texto, "\r\n")] = '\0'; // quitar el salto de linea
            if (strcmp(texto, "exit") == 0)
                break;
            if (texto[0] == '\0')
                continue;
            if (conexion_terminada) // el broker se cayo mientras se escribia
                break;
            snprintf(mensaje, sizeof(mensaje), "%s|%s", partido, texto);
            if (enviar_mensaje(stream, mensaje) == 0)
                printf("Enviado: %s\n", mensaje);
            else
                printf("Error al enviar: %s\n", mensaje);
        }
    }

    //cierre ordenado:
    //StreamShutdown con GRACEFUL envia FIN despues de entregar todo lo pendiente
    //(como close() en TCP). Se espera hasta 5 s a que el broker confirme y cierre su lado,
    //asi no se pierden los ultimos eventos al cerrar la conexion
    if (!conexion_terminada) {
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        esperar_estado(&stream_terminado, 5);
    }

    //ConnectionShutdown cierra la conexion (envia un frame CONNECTION_CLOSE al broker)
    //recibe la conexion, flags (NONE = cierre ordenado) y un codigo de error de aplicacion (0)
    msquic->ConnectionShutdown(conexion, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    esperar_estado(&conexion_terminada, 0);

    //se liberan los handles en orden inverso a su creacion
    msquic->ConnectionClose(conexion);
    msquic->ConfigurationClose(configuracion);
    msquic->RegistrationClose(registro);
    MsQuicClose(msquic);
    printf("Publicador finalizado\n");
    return 0;
}
