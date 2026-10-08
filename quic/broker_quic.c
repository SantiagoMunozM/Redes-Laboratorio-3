#include <stdio.h> // entrada y salida estandar, printf()
#include <stdlib.h> // utilidades generales de C, malloc(), free(), exit()
#include <string.h> // funciones para manipular cadenas, strncmp(), strcpy(), strtok_r()
#include <pthread.h> // mutex para proteger la tabla de clientes (msquic usa varios hilos)
#include <unistd.h> // close()
#include <netinet/in.h> // estructuras para direcciones IP, sockaddr_in
#include <sys/socket.h> // socket(), bind(); solo para revisar si el puerto ya esta ocupado
#include <msquic.h> // libreria msquic: implementacion de QUIC (transporte confiable sobre UDP + TLS 1.3)
#define PUERTO 7000 // puerto UDP donde escucha el broker QUIC
#define MAX_MENSAJE 256 // maximo tamanio de un mensaje (sin contar el '\n')
#define MAX_CLIENTES 50 // maximo de clientes conectados al mismo tiempo
#define MAX_PARTIDOS 8 // maximo de partidos que puede seguir un suscriptor
#define MAX_NOMBRE 50 // maximo tamanio del nombre de un partido
#define TIPO_DESCONOCIDO 0 // el cliente aun no envia su mensaje de registro
#define TIPO_PUBLICADOR 1
#define TIPO_SUSCRIPTOR 2

//ALPN (Application-Layer Protocol Negotiation): nombre del protocolo de aplicacion que se negocia
//durante el handshake TLS. Cliente y broker deben usar el mismo, si no la conexion es rechazada.
//QUIC_BUFFER es una estructura de msquic con dos campos: Length (tamanio) y Buffer (puntero a los bytes)
const QUIC_BUFFER alpn = { sizeof("pubsub") - 1, (uint8_t*)"pubsub" };

//tabla de funciones de msquic. MsQuicOpen2 la llena y todas las llamadas a la libreria
//se hacen a traves de ella (msquic->ListenerOpen, msquic->StreamSend, etc.)
const QUIC_API_TABLE* msquic;

//HQUIC es el tipo "handle" de msquic: un identificador opaco de un objeto de la libreria,
//parecido a como un int identifica un socket en la API de sockets
HQUIC registro; // contexto de ejecucion de la aplicacion dentro de msquic
HQUIC configuracion; // parametros de las conexiones (ALPN, tiempos, certificado TLS)

//cada cliente conectado (publicador o suscriptor) ocupa una posicion de esta tabla
struct Cliente {
    int en_uso; // 1 si la posicion esta ocupada
    HQUIC conexion; // conexion QUIC con el cliente (equivale al descriptor de accept() en TCP)
    HQUIC stream; // stream bidireccional por donde viajan los mensajes con este cliente
    int tipo; // TIPO_DESCONOCIDO, TIPO_PUBLICADOR o TIPO_SUSCRIPTOR
    char partidos[MAX_PARTIDOS][MAX_NOMBRE]; // partidos que sigue (suscriptor) o reporta (publicador)
    int num_partidos;
    char buffer[MAX_MENSAJE + 1]; // bytes recibidos que todavia no forman un mensaje completo
    int bytes_buffer;
};
struct Cliente clientes[MAX_CLIENTES];

//msquic ejecuta los callbacks en sus propios hilos (varios a la vez), por eso dos publicadores
//pueden estar leyendo/escribiendo la tabla al mismo tiempo. El mutex evita esa condicion de carrera.
//(en el broker TCP con select() no hace falta porque todo corre en un solo hilo)
pthread_mutex_t mutex_clientes = PTHREAD_MUTEX_INITIALIZER;


//revisa si otro proceso ya usa el puerto UDP. msquic abre sus sockets con SO_REUSEPORT,
//asi que un segundo broker en el mismo puerto NO da error y el sistema operativo reparte los
//clientes entre ambos. Para evitarlo se intenta un bind() normal (sin SO_REUSEPORT), que si
//falla cuando el puerto ya esta ocupado. Devuelve 1 si esta ocupado y 0 si esta libre.
int puerto_ocupado(int puerto)
{
    struct sockaddr_in direccion;
    //socket UDP (SOCK_DGRAM) IPv4, igual que en la version UDP del laboratorio
    int descriptor_prueba = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptor_prueba == -1)
        return 0; // no se pudo revisar; se deja que msquic lo intente
    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_addr.s_addr = htonl(INADDR_ANY);
    direccion.sin_port = htons(puerto);
    int ocupado = (bind(descriptor_prueba, (struct sockaddr*)&direccion, sizeof(direccion)) == -1);
    close(descriptor_prueba); // se libera de inmediato, solo era una prueba
    return ocupado;
}

//envia un mensaje (ya terminado en '\n') por un stream
void enviar_mensaje(HQUIC stream, const char* mensaje)
{
    int longitud = strlen(mensaje);

    //StreamSend es asincrono: msquic se queda con el puntero y envia los bytes despues,
    //por eso el mensaje no puede estar en una variable local. Se reserva en un solo bloque
    //la estructura QUIC_BUFFER seguida de una copia del mensaje.
    QUIC_BUFFER* buffer_envio = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) + longitud);
    if (buffer_envio == NULL) {
        printf("Error: sin memoria para enviar el mensaje\n");
        return;
    }
    buffer_envio->Buffer = (uint8_t*)(buffer_envio + 1); // los bytes van justo despues de la estructura
    buffer_envio->Length = longitud;
    memcpy(buffer_envio->Buffer, mensaje, longitud);

    //StreamSend encola datos para enviarlos por el stream (equivale a send()/write() en TCP)
    //recibe cinco parametros:
    // el stream por el que se envia
    // un arreglo de QUIC_BUFFER con los datos y la cantidad de buffers del arreglo (1)
    // flags: QUIC_SEND_FLAG_NONE envia normal (QUIC_SEND_FLAG_FIN cerraria el envio)
    // un contexto propio que msquic devuelve en el evento SEND_COMPLETE; pasamos el mismo
    //    buffer para poder liberarlo cuando msquic termine de usarlo
    //devuelve un QUIC_STATUS; la macro QUIC_FAILED indica si hubo error
    if (QUIC_FAILED(msquic->StreamSend(stream, buffer_envio, 1, QUIC_SEND_FLAG_NONE, buffer_envio))) {
        printf("Error al enviar por el stream\n");
        free(buffer_envio); // si fallo, msquic no generara SEND_COMPLETE, se libera aqui
    }
}

//reenvia una noticia a todos los suscriptores del partido (se llama con el mutex tomado)
void reenviar_noticia(const char* partido, const char* mensaje)
{
    int enviados = 0;
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!clientes[i].en_uso || clientes[i].tipo != TIPO_SUSCRIPTOR || clientes[i].stream == NULL)
            continue;
        for (int j = 0; j < clientes[i].num_partidos; j++) {
            if (strcmp(clientes[i].partidos[j], partido) == 0) {
                //el broker no modifica el contenido: reenvia exactamente la misma linea
                enviar_mensaje(clientes[i].stream, mensaje);
                enviados++;
                break;
            }
        }
    }
    printf("Noticia de %s reenviada a %d suscriptor(es)\n", partido, enviados);
}

//procesa un mensaje completo (sin el '\n') recibido de un cliente (se llama con el mutex tomado)
void procesar_mensaje(struct Cliente* cliente, char* linea)
{
    char copia[MAX_MENSAJE + 2];

    //registro de suscriptor: "SUBSCRIBER|partido1,partido2,..."
    if (strncmp(linea, "SUBSCRIBER|", 11) == 0) {
        char* resto;
        char* partido;
        cliente->tipo = TIPO_SUSCRIPTOR;
        cliente->num_partidos = 0;
        //strtok_r separa la lista por comas (version segura para hilos de strtok)
        partido = strtok_r(linea + 11, ",", &resto);
        while (partido != NULL && cliente->num_partidos < MAX_PARTIDOS) {
            strncpy(cliente->partidos[cliente->num_partidos], partido, MAX_NOMBRE - 1);
            cliente->partidos[cliente->num_partidos][MAX_NOMBRE - 1] = '\0';
            cliente->num_partidos++;
            partido = strtok_r(NULL, ",", &resto);
        }
        printf("Nuevo suscriptor registrado, sigue %d partido(s):", cliente->num_partidos);
        for (int j = 0; j < cliente->num_partidos; j++)
            printf(" %s", cliente->partidos[j]);
        printf("\n");

        //confirmacion para el suscriptor: "OK|partido1,partido2"
        snprintf(copia, sizeof(copia), "OK|");
        for (int j = 0; j < cliente->num_partidos; j++) {
            if (j > 0) strncat(copia, ",", sizeof(copia) - strlen(copia) - 1);
            strncat(copia, cliente->partidos[j], sizeof(copia) - strlen(copia) - 1);
        }
        strncat(copia, "\n", sizeof(copia) - strlen(copia) - 1);
        enviar_mensaje(cliente->stream, copia);
    }
    //registro de publicador: "PUBLISHER|partido"
    else if (strncmp(linea, "PUBLISHER|", 10) == 0) {
        cliente->tipo = TIPO_PUBLICADOR;
        strncpy(cliente->partidos[0], linea + 10, MAX_NOMBRE - 1);
        cliente->partidos[0][MAX_NOMBRE - 1] = '\0';
        cliente->num_partidos = 1;
        printf("Nuevo publicador registrado para el partido %s\n", cliente->partidos[0]);
        snprintf(copia, sizeof(copia), "OK|%s\n", cliente->partidos[0]);
        enviar_mensaje(cliente->stream, copia);
    }
    //noticia de un publicador: "partido|texto"
    else if (cliente->tipo == TIPO_PUBLICADOR) {
        char partido[MAX_NOMBRE];
        //sscanf con %49[^|] lee hasta 49 caracteres que no sean '|' (el nombre del partido)
        if (sscanf(linea, "%49[^|]", partido) != 1 || strchr(linea, '|') == NULL) {
            printf("Noticia con formato invalido, se descarta: %s\n", linea);
            return;
        }
        printf("Noticia recibida: %s\n", linea);
        //se vuelve a agregar el '\n' que delimita el mensaje en el stream
        snprintf(copia, sizeof(copia), "%s\n", linea);
        reenviar_noticia(partido, copia);
    }
    else {
        printf("Mensaje ignorado (cliente no registrado como publicador): %s\n", linea);
    }
}


//callback del stream: msquic lo llama cada vez que ocurre algo en el stream de un cliente
//el contexto es el puntero a la posicion del cliente en la tabla
QUIC_STATUS QUIC_API callback_stream(HQUIC stream, void* contexto, QUIC_STREAM_EVENT* evento)
{
    struct Cliente* cliente = (struct Cliente*)contexto;

    switch (evento->Type) {

    //llegaron datos (equivale a que recv()/read() devuelva bytes en TCP)
    //igual que en TCP, un stream QUIC es un flujo de bytes: un mensaje puede llegar partido
    //o pegado con otro, por eso se acumulan los bytes y se corta en cada '\n'
    case QUIC_STREAM_EVENT_RECEIVE:
        pthread_mutex_lock(&mutex_clientes);
        //los datos vienen en un arreglo de QUIC_BUFFER (BufferCount buffers)
        for (uint32_t i = 0; i < evento->RECEIVE.BufferCount; i++) {
            const QUIC_BUFFER* recibido = &evento->RECEIVE.Buffers[i];
            for (uint32_t k = 0; k < recibido->Length; k++) {
                char c = (char)recibido->Buffer[k];
                if (c == '\n') {
                    cliente->buffer[cliente->bytes_buffer] = '\0';
                    //se quita un posible '\r' final (si el texto viene de Windows)
                    if (cliente->bytes_buffer > 0 && cliente->buffer[cliente->bytes_buffer - 1] == '\r')
                        cliente->buffer[cliente->bytes_buffer - 1] = '\0';
                    if (cliente->buffer[0] != '\0')
                        procesar_mensaje(cliente, cliente->buffer);
                    cliente->bytes_buffer = 0;
                }
                else if (cliente->bytes_buffer < MAX_MENSAJE) {
                    cliente->buffer[cliente->bytes_buffer++] = c;
                }
                //si el mensaje supera MAX_MENSAJE, los bytes sobrantes se descartan
            }
        }
        pthread_mutex_unlock(&mutex_clientes);
        break;

    //msquic termino de enviar un StreamSend (o lo cancelo): ya se puede liberar el buffer
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        free(evento->SEND_COMPLETE.ClientContext);
        break;

    //el cliente cerro su direccion de envio (envio FIN, como cuando se hace close() en TCP)
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        //StreamShutdown cierra nuestra direccion de envio del stream
        //recibe el stream, flags (GRACEFUL = cerrar ordenadamente despues de entregar lo pendiente)
        //y un codigo de error de aplicacion (0, solo se usa al abortar)
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        break;

    //el cliente aborto el stream de forma abrupta
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        msquic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;

    //ambas direcciones del stream terminaron: ya se puede liberar el handle
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        //primero se quita el stream de la tabla (con el mutex) para que ningun otro hilo
        //intente reenviar por un stream que se esta cerrando
        pthread_mutex_lock(&mutex_clientes);
        cliente->stream = NULL;
        pthread_mutex_unlock(&mutex_clientes);
        //StreamClose libera el handle del stream
        msquic->StreamClose(stream);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//callback de la conexion: msquic lo llama con los eventos de la conexion con un cliente
QUIC_STATUS QUIC_API callback_conexion(HQUIC conexion, void* contexto, QUIC_CONNECTION_EVENT* evento)
{
    struct Cliente* cliente = (struct Cliente*)contexto;

    switch (evento->Type) {

    //termino el handshake QUIC + TLS 1.3: la conexion queda establecida
    //(a diferencia de TCP, transporte y cifrado se negocian en el mismo handshake de 1 RTT)
    case QUIC_CONNECTION_EVENT_CONNECTED:
        printf("Conexion QUIC establecida con un cliente\n");
        break;

    //el cliente abrio un stream: por ahi enviara su registro y sus mensajes
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        pthread_mutex_lock(&mutex_clientes);
        cliente->stream = evento->PEER_STREAM_STARTED.Stream;
        pthread_mutex_unlock(&mutex_clientes);
        //SetCallbackHandler asocia a un handle la funcion que atendera sus eventos y un contexto
        //recibe el handle (el stream nuevo), la funcion callback y el contexto (el cliente)
        msquic->SetCallbackHandler(evento->PEER_STREAM_STARTED.Stream, (void*)callback_stream, cliente);
        break;

    //la conexion se cerro por el transporte (timeout de inactividad, cliente caido, etc.)
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        //en Linux los QUIC_STATUS son codigos errno, strerror los traduce a texto
        printf("Conexion con un cliente cerrada por el transporte: %s\n",
               strerror(evento->SHUTDOWN_INITIATED_BY_TRANSPORT.Status));
        break;

    //el cliente cerro la conexion
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        printf("El cliente cerro la conexion\n");
        break;

    //la conexion termino por completo: se libera el handle y la posicion de la tabla
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        pthread_mutex_lock(&mutex_clientes);
        if (cliente->tipo == TIPO_SUSCRIPTOR)
            printf("Suscriptor desconectado\n");
        else if (cliente->tipo == TIPO_PUBLICADOR)
            printf("Publicador de %s desconectado\n", cliente->partidos[0]);
        cliente->en_uso = 0;
        pthread_mutex_unlock(&mutex_clientes);
        //ConnectionClose libera el handle de la conexion (equivale a close() del descriptor)
        msquic->ConnectionClose(conexion);
        break;

    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//callback del listener: msquic lo llama cuando llega una conexion nueva
//(equivale a que accept() devuelva un descriptor nuevo en TCP)
QUIC_STATUS QUIC_API callback_listener(HQUIC listener, void* contexto, QUIC_LISTENER_EVENT* evento)
{
    (void)listener; (void)contexto; // no se usan

    if (evento->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION)
        return QUIC_STATUS_SUCCESS;

    //se busca una posicion libre en la tabla para el nuevo cliente
    struct Cliente* cliente = NULL;
    pthread_mutex_lock(&mutex_clientes);
    for (int i = 0; i < MAX_CLIENTES; i++) {
        if (!clientes[i].en_uso) {
            cliente = &clientes[i];
            memset(cliente, 0, sizeof(*cliente));
            cliente->en_uso = 1;
            cliente->conexion = evento->NEW_CONNECTION.Connection;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_clientes);

    //si no hay espacio se rechaza la conexion
    if (cliente == NULL) {
        printf("Tabla de clientes llena, conexion rechazada\n");
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    //asociamos el callback de la conexion, con el cliente como contexto
    msquic->SetCallbackHandler(evento->NEW_CONNECTION.Connection, (void*)callback_conexion, cliente);

    //ConnectionSetConfiguration le asigna a la conexion entrante la configuracion del servidor
    //(certificado, ALPN, tiempos); con esto msquic continua el handshake TLS con el cliente
    //recibe la conexion y la configuracion; devuelve un QUIC_STATUS
    return msquic->ConnectionSetConfiguration(evento->NEW_CONNECTION.Connection, configuracion);
}


int main(int argc, char* argv[])
{
    QUIC_STATUS estado; // codigo de resultado de las funciones de msquic
    HQUIC listener = NULL; // "oido" del broker (equivale al socket de escucha en TCP)
    const char* archivo_certificado = "server.cert";
    const char* archivo_llave = "server.key";

    //opcionalmente se pueden indicar otras rutas: ./broker_quic certificado llave
    if (argc == 3) {
        archivo_certificado = argv[1];
        archivo_llave = argv[2];
    }

    if (puerto_ocupado(PUERTO)) {
        printf("Error: el puerto UDP %d ya esta en uso (hay otro broker abierto?)\n", PUERTO);
        exit(0);
    }

    //MsQuicOpen2 carga la libreria y llena la tabla de funciones de la API (version 2)
    //recibe un puntero donde dejar la tabla; devuelve un QUIC_STATUS
    if (QUIC_FAILED(estado = MsQuicOpen2(&msquic))) {
        printf("Error al abrir msquic (0x%x)\n", (unsigned int)estado);
        exit(0);
    }
    else
        printf("Libreria msquic cargada correctamente..\n");

    //RegistrationOpen crea el registro: el contexto de ejecucion de la aplicacion
    //(hilos de trabajo de msquic). Recibe una configuracion con:
    // AppName: nombre de la aplicacion (solo informativo)
    // ExecutionProfile: LOW_LATENCY prioriza la latencia (perfil por defecto)
    //y un puntero donde dejar el handle del registro
    const QUIC_REGISTRATION_CONFIG config_registro = { "broker_quic", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    if (QUIC_FAILED(estado = msquic->RegistrationOpen(&config_registro, &registro))) {
        printf("Error al crear el registro (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //QUIC_SETTINGS contiene los parametros de las conexiones. Solo se aplican los campos
    //marcados con IsSet; el resto queda con su valor por defecto.
    QUIC_SETTINGS parametros;
    memset(&parametros, 0, sizeof(parametros));
    //IdleTimeoutMs: si no llega nada en 30 s la conexion se da por muerta
    //(asi el broker detecta suscriptores que se cayeron sin cerrar la conexion)
    parametros.IdleTimeoutMs = 30000;
    parametros.IsSet.IdleTimeoutMs = TRUE;
    //PeerBidiStreamCount: cuantos streams bidireccionales puede abrir cada cliente (usamos 1)
    parametros.PeerBidiStreamCount = 1;
    parametros.IsSet.PeerBidiStreamCount = TRUE;

    //ConfigurationOpen crea la configuracion que usaran las conexiones
    //recibe siete parametros:
    // el registro al que pertenece
    // la lista de ALPN aceptados y cuantos son (1: "pubsub")
    // los parametros (QUIC_SETTINGS) y su tamanio
    // un contexto propio (NULL, no se usa)
    // un puntero donde dejar el handle de la configuracion
    if (QUIC_FAILED(estado = msquic->ConfigurationOpen(registro, &alpn, 1, &parametros, sizeof(parametros), NULL, &configuracion))) {
        printf("Error al crear la configuracion (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //QUIC siempre va cifrado con TLS 1.3, por eso el broker (servidor) necesita un certificado
    //y su llave privada. QUIC_CREDENTIAL_CONFIG describe de donde sacarlos:
    // Type: CERTIFICATE_FILE indica que estan en archivos PEM (generados con openssl)
    // Flags: NONE (sin la flag CLIENT, msquic entiende que es un servidor)
    // CertificateFile: estructura con las rutas del certificado y de la llave
    QUIC_CERTIFICATE_FILE archivos_certificado;
    archivos_certificado.CertificateFile = archivo_certificado;
    archivos_certificado.PrivateKeyFile = archivo_llave;
    QUIC_CREDENTIAL_CONFIG credenciales;
    memset(&credenciales, 0, sizeof(credenciales));
    credenciales.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    credenciales.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    credenciales.CertificateFile = &archivos_certificado;

    //ConfigurationLoadCredential carga el certificado TLS en la configuracion
    //recibe la configuracion y las credenciales; devuelve un QUIC_STATUS
    if (QUIC_FAILED(estado = msquic->ConfigurationLoadCredential(configuracion, &credenciales))) {
        printf("Error al cargar el certificado %s / %s (0x%x)\n", archivo_certificado, archivo_llave, (unsigned int)estado);
        printf("Genere el certificado con: openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.cert -days 365 -subj \"/CN=broker\"\n");
        exit(0);
    }
    else
        printf("Certificado TLS cargado correctamente..\n");

    //ListenerOpen crea el listener (el objeto que recibe conexiones nuevas)
    //recibe el registro, la funcion callback que atendera las conexiones nuevas,
    //un contexto (NULL) y un puntero donde dejar el handle del listener
    if (QUIC_FAILED(estado = msquic->ListenerOpen(registro, callback_listener, NULL, &listener))) {
        printf("Error al crear el listener (0x%x)\n", (unsigned int)estado);
        exit(0);
    }

    //QUIC_ADDR es la direccion local donde escuchar (envuelve a sockaddr_in / sockaddr_in6)
    //QuicAddrSetFamily con UNSPEC + sin IP = escuchar en todas las interfaces (como INADDR_ANY)
    //QuicAddrSetPort asigna el puerto (la funcion ya hace la conversion a orden de red, como htons)
    QUIC_ADDR direccion_broker;
    memset(&direccion_broker, 0, sizeof(direccion_broker));
    QuicAddrSetFamily(&direccion_broker, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&direccion_broker, PUERTO);

    //ListenerStart empieza a escuchar conexiones (equivale a bind() + listen() en TCP)
    //por debajo msquic crea un socket UDP asociado al puerto 7000
    //recibe el listener, la lista de ALPN y su cantidad, y la direccion local
    if (QUIC_FAILED(estado = msquic->ListenerStart(listener, &alpn, 1, &direccion_broker))) {
        printf("Error al iniciar el listener en el puerto %d (0x%x)\n", PUERTO, (unsigned int)estado);
        exit(0);
    }
    else
        printf("Broker QUIC escuchando en el puerto UDP %d..\n", PUERTO);

    //a partir de aqui todo lo hacen los callbacks en los hilos de msquic
    //el hilo principal solo espera a que el usuario presione Enter para apagar el broker
    printf("Presione Enter para detener el broker\n");
    getchar();

    //apagado ordenado:
    //ListenerClose deja de aceptar conexiones y libera el listener
    msquic->ListenerClose(listener);
    //RegistrationShutdown cierra todas las conexiones abiertas del registro
    //(los clientes reciben el evento SHUTDOWN_INITIATED_BY_PEER)
    //recibe el registro, flags (NONE = cierre ordenado) y un codigo de error de aplicacion (0)
    msquic->RegistrationShutdown(registro, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    //ConfigurationClose y RegistrationClose liberan sus handles
    //(RegistrationClose espera a que todas las conexiones terminen de cerrarse)
    msquic->ConfigurationClose(configuracion);
    msquic->RegistrationClose(registro);
    //MsQuicClose descarga la libreria
    MsQuicClose(msquic);
    printf("Broker detenido\n");
    return 0;
}
