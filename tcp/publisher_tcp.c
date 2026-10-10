#include <stdio.h> // entrada y salida estandar, printf()
#include <arpa/inet.h> // inet_addr(), htons()
#include <netdb.h> // definiciones para operaciones de red
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <strings.h> // bzero()
#include <signal.h>
#include <sys/socket.h> // libreria principal para los sockets
#include <unistd.h> // read(), write(), close()
#define PORT 8080
#define SA struct sockaddr  // alias para struct sockaddr
#define MAX_MENSAJE 512
#define NUM_EVENTOS 10

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

/*
 * Envia una linea al broker agregandole el '\n'
 * que el broker usa como delimitador de mensajes.
 *
 * Retorna 0 si se envio completa y -1 si hubo error.
 */
int enviar_linea(int descriptor_socket, const char *mensaje)
{
    //declaramos un arreglo de caracteres para almacenar el mensaje a enviar al broker
    //le damos dos bytes mas que el tamanio maximo para el salto de linea y el terminador nulo
    char linea[MAX_MENSAJE + 2];

    //usamos la funcion snprintf de la libreria stdio.h para formatear el mensaje a enviar al broker
    //snprintf nos permite copiar el contenido de mensaje en el buffer y agregar un salto de linea al final
    //la funcion recibe cuatro parametros:
    // un puntero al buffer donde se almacenara el mensaje formateado (linea)
    // el tamanio del mensaje (sizeof(linea))
    // la cadena de formato que indica como formatear el mensaje ("%s\n")
    // la cadena de caracteres a formatear (mensaje)
    //el retorno es el numero de caracteres que se habrian escrito en el buffer.
    int n = snprintf(linea, sizeof(linea), "%s\n", mensaje);

    //usamos un ciclo para que el envio sea completo, pues si el buffer del socket esta casi lleno, send hace envios parciales
    //asi, el bucle revisa cuantos bytes se enviaron y envia los restantes hasta que se complete el envio
    int enviados = 0;
    while (enviados < n) {
        //usamos la funcion send de la libreria sys/socket.h para enviar el mensaje al broker
        //esta funcion envia datos a un socket y devuelve el numero de bytes enviados (-1 si error)
        //la funcion recibe cuatro parametros:
        // el descriptor del socket del publicador conectado al broker, para saber a que socket enviar
        // un puntero al buffer que contiene los datos a enviar (linea+enviados, primer byte no enviado)
        // el tamanio del buffer (n - enviados)
        // un entero que indica las opciones de la funcion (0 para comportamiento por defecto)
        int r =send(descriptor_socket, linea + enviados, n - enviados, 0);

        //el retorno de send es el numero de bytes que se enviaron, si es negativo hubo un error y retornamos -1
        //el error ya no termina el programa, pues usamos signal(SIGPIPE, SIG_IGN) para ignorar la señal SIGPIPE y usamos
        //la siguiente verificacion para capturar el error de send cuando llega la señal SIGPIPE
        if (r < 0)
            return -1;
        
        //si no fue negativo agregamos los bytes enviados al contador
        enviados += r;
    }
    return 0;
}

/*
 * Modo automatico: envia los 10 eventos de ejemplo
 * del partido, uno por segundo.
 */
void modo_automatico(int descriptor_socket, const char *partido)
{
    //declarar arreglo del mensaje
    char mensaje[MAX_MENSAJE];

    //ciclo para los eventos a enviar
    for (int i = 0; i < NUM_EVENTOS; i++) {
        //formateamos el mensaje a enviar al broker
        snprintf(mensaje, sizeof(mensaje), "%s|[%02d] %s", partido, i + 1, eventos[i]);

        //intentamos enviar la linea al broker, si hay error notificamos y salimos de la funcion
        if (enviar_linea(descriptor_socket, mensaje) < 0) {
            printf("Error al enviar el mensaje, el broker cerro la conexion\n");
            return;
        }
        printf("Enviado %d/%d: %s\n", i + 1, NUM_EVENTOS, mensaje);

        //esperamos un segundo antes de enviar el siguiente evento
        sleep(1);
    }
    printf("Los %d eventos fueron enviados\n", NUM_EVENTOS);
}

/*
 * Modo manual: cada linea escrita por el periodista
 * es un evento. Termina al escribir "exit" o con Ctrl+D.
 */
void modo_manual(int descriptor_socket, const char *partido)
{
    //arreglo que guardara el texto escrito en consola
    char texto[MAX_MENSAJE - 60];

    //arreglo que guardara el mensaje a enviar al broker, incluyendo el partido y el texto
    char mensaje[MAX_MENSAJE];

    printf("Escriba un evento por linea ('exit' para terminar):\n");
    //usando la funcion fgets de la libreria stdio.h leemos una linea de texto por entrada estandar y la almacenamos en el arreglo texto
    //fgets lee como maximo el tamanio de texto - 1, el resto queda pendiente en el buffer de entrada estandar
    //el ciclo se repite mientras la funcion no retorne NULL, lo que indica que se llego al final de la entrada (Ctrl+D) o hubo un error
    while (fgets(texto, sizeof(texto), stdin) != NULL) {
        //reemplazamos el salto de linea al final de la cadena por un terminador nulo para que sea una cadena valida
        texto[strcspn(texto, "\n")] = '\0';

        //si el texto es "exit", salimos del ciclo y terminamos la funcion
        if (strcmp(texto, "exit") == 0)
            break;
        //si el texto es vacio, no hacemos nada y seguimos al siguiente ciclo
        if (texto[0] == '\0')
            continue;
        
        //formateamos el mensaje a enviar al broker, agregando el partido y el texto del evento
        snprintf(mensaje, sizeof(mensaje), "%s|%s", partido, texto);

        //enviamos el mensaje al broker, si hay error notificamos y salimos de la funcion
        if (enviar_linea(descriptor_socket, mensaje) < 0) {
            printf("Error al enviar el mensaje, el broker cerro la conexion\n");
            return;
        }
        printf("Enviado: %s\n", mensaje);
    }
}

/*
 * Se conecta al broker, se registra como PUBLISHER
 * y publica los eventos del partido indicado.
 *
 * Uso: ./publisher_tcp <ip_broker> <partido> [auto|manual]
 */
int main(int argc, char *argv[])
{
    //verificamos uso correcto de argumentos
    if (argc < 3) {
        printf("Uso: %s <ip_broker> <partido> [auto|manual]\n", argv[0]);
        printf("Ejemplo: %s 192.168.1.20 ColombiaVsBrasil auto\n", argv[0]);
        return 1;
    }

    //obtenemos la IP del broker, el partido y el modo de publicacion
    const char *ip_broker = argv[1];
    const char *partido = argv[2];
    int modo_manual_activo = (argc >= 4 && strcmp(argv[3], "manual") == 0);

    //inet_addr devuelve INADDR_NONE cuando el texto recibido no es una direccion IPv4 valida
    if (inet_addr(ip_broker) == INADDR_NONE) {
        printf("IP del broker invalida: %s\n", ip_broker);
        return 1;
    }


    //usando la funcion strchr de la libreria string.h rechazamos si el partido contiene un '|' 
    //usando la funcion strlen de la libreria string.h rechazamos si el partido tiene mas de 49 caracteres o es vacio
    if (strchr(partido, '|') != NULL || strlen(partido) > 49 || strlen(partido) == 0) {
        printf("Partido invalido: no puede tener '|' ni mas de 49 caracteres\n");
        return 1;
    }

    int descriptor_socket; // descriptor del socket del cliente

    // variable que almacena la direccion del servidor al que nos conectaremos
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto de la conexion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;

    //signal es una funcion de la libreria signal.h que evita que el publisher deje
    //de funcionar cuando reciba una señal SIGPIPE. Se declara al comienzo y es valido durante toda la ejecucion.
    //una señal SIGPIPE se genera cuando el publisher intenta escribir en un socket que
    //ha sido cerrado por el servidor y que el publisher no ha detectado.
    signal(SIGPIPE, SIG_IGN);

    // crear el socket
    //usamos la funcion socket de la libreria sys/socket.h
    //esta funcion le pide al sistema operativo que cree un socket
    //y nos devuelve un descriptor que representa el socket, pero todavia no esta conectado
    //a ningun servidor. Los parametros usados cumplen la siguiente funcion:
    // AF_INET: indica que se usara el protocolo IPv4 (familia de direcciones)
    // SOCK_STREAM: indica que debe ser un flujo confiable (TCP)
    // 0: indica usar el protocolo por defecto para la combinacion de protocolo IP y tipo de flujo.
    descriptor_socket = socket(AF_INET, SOCK_STREAM, 0);

    //si el descriptor es -1, significa que hubo un error al crear el socket
    if (descriptor_socket == -1) {
        printf("Error al crear el socket\n");
        exit(0);
    }
    else
        printf("Socket creado correctamente..\n");

    //una vez creado el socket, usamos la funcion bzero de la libreria strings.h para inicializar la
    //estructura de direccion del servidor en 0s
    bzero(&direccion_servidor, sizeof(direccion_servidor));


    //una vez creado el socket, configuramos la estructura direccion_servidor
    //con los datos del servidor al que queremos conectarnos

    //declaramos la familia de direcciones
    direccion_servidor.sin_family = AF_INET;

    //asignamos la direccion IP del servidor
    //sin_addr es una estructura que contiene la direccion IP del servidor
    //s_addr es el campo de la estructura sin_addr que contiene la direccion IP en formato binario
    //ip_broker es la IP del broker recibida como primer argumento (127.0.0.1 si corre en esta misma maquina)
    //inet_addr convierte la direccion IP escrita como texto (por ejemplo "192.168.1.20") a formato binario
    direccion_servidor.sin_addr.s_addr = inet_addr(ip_broker);

    //asignamos el puerto del servidor
    //htons convierte el puerto de formato de host a formato de red
    direccion_servidor.sin_port = htons(PORT);

    //el siguiente paso es conectar el socket del cliente con el socket del servidor
    //para esto utilizamos la funcion connect de la libreria sys/socket.h
    //connect recibe tres parametros:
    // el descriptor del socket, para saber que socket conectar
    // un puntero a la direccion del servidor al que se quiere conectar (direccion_servidor)
    // el tamaño de la estructura de direccion (sizeof(direccion_servidor))
    //    necesario dado que la estructura se recibe por puntero y el tamanio permite leer correctamente
    //si la conexion fue exitosa, connect devuelve 0, si hubo un error devuelve -1
    if (connect(descriptor_socket, (SA*)&direccion_servidor, sizeof(direccion_servidor)) == 0) {
        //conexion exitosa
        printf("Conectado al servidor correctamente..\n");
    }
    else {
        //falla en la conexion, cerramos el socket y salimos del programa
        printf("Error al conectar con el servidor\n");
        close(descriptor_socket);
        exit(0);
    }

    //una vez conectado, nos registramos como publicador enviando la cadena "PUBLISHER" al broker
    if (enviar_linea(descriptor_socket, "PUBLISHER") < 0) {
        //si hubo error al enviar, cerramos el socket y salimos del programa
        printf("Error al registrarse como publisher\n");
        close(descriptor_socket);
        return 1;
    }
    printf("Registrado como PUBLISHER del partido %s\n", partido);

    //dependiendo del modo de publicacion, llamamos a la funcion correspondiente
    if (modo_manual_activo)
        modo_manual(descriptor_socket, partido);
    else
        modo_automatico(descriptor_socket, partido);

    close(descriptor_socket);
    return 0;
}
