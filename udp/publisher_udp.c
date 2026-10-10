#include <stdio.h> // entrada y salida estandar, printf()
#include <arpa/inet.h> // inet_addr(), htons()
#include <netdb.h> // definiciones para operaciones de red
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <strings.h> // bzero()
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
 * Envia un datagrama al broker con el mensaje indicado.
 *
 * Retorna 0 si se envio y -1 si hubo error.
 */
int enviar_datagrama(int descriptor_socket, struct sockaddr_in *direccion_servidor, const char *mensaje)
{
    if (sendto(descriptor_socket, mensaje, strlen(mensaje), 0,
               (SA*)direccion_servidor, sizeof(*direccion_servidor)) < 0)
        return -1;
    return 0;
}

/*
 * Modo automatico: envia los 10 eventos de ejemplo
 * del partido, uno por segundo.
 */
void modo_automatico(int descriptor_socket, struct sockaddr_in *direccion_servidor, const char *partido)
{
    //declarar arreglo del mensaje
    char mensaje[MAX_MENSAJE];

    //ciclo para los eventos a enviar
    for (int i = 0; i < NUM_EVENTOS; i++) {
        //formateamos el mensaje a enviar al broker
        snprintf(mensaje, sizeof(mensaje), "%s|[%02d] %s", partido, i + 1, eventos[i]);

        //intentamos enviar el mensaje al broker, si hay error notificamos y salimos de la funcion
        if (enviar_datagrama(descriptor_socket, direccion_servidor, mensaje) < 0) {
            printf("Error al enviar el mensaje\n");
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
void modo_manual(int descriptor_socket, struct sockaddr_in *direccion_servidor, const char *partido)
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
        if (enviar_datagrama(descriptor_socket, direccion_servidor, mensaje) < 0) {
            printf("Error al enviar el mensaje\n");
            return;
        }
        printf("Enviado: %s\n", mensaje);
    }
}

/*
 * Se registra ante el broker como PUBLISHER
 * y publica los eventos del partido indicado.
 *
 * Uso: ./publisher_udp <ip_broker> <partido> [auto|manual]
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

    // variable que almacena la direccion del servidor al que enviaremos los datagramas
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto de la conexion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;

    // crear el socket
    //usamos la funcion socket de la libreria sys/socket.h
    //esta funcion le pide al sistema operativo que cree un socket
    //y nos devuelve un descriptor que representa el socket.
    //Los parametros usados cumplen la siguiente funcion:
    // AF_INET: indica que se usara el protocolo IPv4 (familia de direcciones)
    // 0: indica usar el protocolo por defecto para la combinacion de protocolo IP y tipo de flujo.
    descriptor_socket = socket(AF_INET, SOCK_DGRAM, 0);

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
    //con los datos del servidor al que queremos enviar los datagramas

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

    //nos registramos como publicador enviando "PUBLISHER|partido" al broker
    char registro[MAX_MENSAJE];
    snprintf(registro, sizeof(registro), "PUBLISHER|%s", partido);
    if (enviar_datagrama(descriptor_socket, &direccion_servidor, registro) < 0) {
        //si hubo error al enviar, cerramos el socket y salimos del programa
        printf("Error al registrarse como publisher\n");
        close(descriptor_socket);
        return 1;
    }
    printf("Registrado como PUBLISHER del partido %s\n", partido);

    //dependiendo del modo de publicacion, llamamos a la funcion correspondiente
    if (modo_manual_activo)
        modo_manual(descriptor_socket, &direccion_servidor, partido);
    else
        modo_automatico(descriptor_socket, &direccion_servidor, partido);

    close(descriptor_socket);
    return 0;
}
