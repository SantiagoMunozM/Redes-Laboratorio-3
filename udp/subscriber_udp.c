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
#define MAX_BUFFER 1024
#define MAX_PARTIDOS 10

/*
 * Recibe las noticias que reenvia el broker y las
 * imprime, una por datagrama. No termina por si sola:
 * se detiene con Ctrl+C.
 */
void recibir_noticias(int descriptor_socket)
{
    char buffer[MAX_BUFFER];

    for (;;) {
        int n = recvfrom(descriptor_socket, buffer, MAX_BUFFER - 1, 0, NULL, NULL);
        if (n < 0) {
            printf("Error al recibir el datagrama\n");
            continue;
        }

        //agregamos un terminador nulo al final del buffer para saber hasta donde llega el mensaje recibido
        buffer[n] = '\0';
        printf("Noticia recibida: %s\n", buffer);
    }
}

/*
 * Se registra ante el broker en los partidos
 * indicados y muestra las noticias que le reenvia.
 *
 * Uso: ./subscriber_udp <ip_broker> <partido1> [partido2 ...]
 */
int main(int argc, char *argv[])
{
    //verificamos uso correcto de argumentos
    if (argc < 3 || argc - 2 > MAX_PARTIDOS) {
        printf("Uso: %s <ip_broker> <partido1> [partido2 ...] (maximo %d partidos)\n", argv[0], MAX_PARTIDOS);
        printf("Ejemplo: %s 192.168.1.20 ColombiaVsBrasil ArgentinaVsChile\n", argv[0]);
        return 1;
    }

    //obtenemos la IP del broker; los partidos empiezan en el segundo argumento
    const char *ip_broker = argv[1];

    //inet_addr devuelve INADDR_NONE cuando el texto recibido no es una direccion IPv4 valida
    if (inet_addr(ip_broker) == INADDR_NONE) {
        printf("IP del broker invalida: %s\n", ip_broker);
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        //usando la funcion strchr de la libreria string.h rechazamos si el partido contiene un '|'
        //usando la funcion strlen de la libreria string.h rechazamos si el partido tiene mas de 49 caracteres o es vacio
        if (strchr(argv[i], '|') != NULL || strlen(argv[i]) > 49 || strlen(argv[i]) == 0) {
            printf("Partido invalido: no puede tener '|' ni mas de 49 caracteres\n");
            return 1;
        }
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

    //revisa los partidos indicados en los argumentos, envia un mensaje de suscripcion al broker por cada uno
    for (int i = 2; i < argc; i++) {
        char registro[MAX_MENSAJE];

        //usamos la funcion snprintf de la libreria stdio.h para formatear el mensaje de suscripcion al broker
        //la funcion nos permite copiar el contenido del i-esimo partido en el buffer y agregarle el prefijo de reconocimiento de suscriptor
        //asi, el mensaje tiene el formato correcto para el broker
        snprintf(registro, sizeof(registro), "SUBSCRIBER|%s", argv[i]);

        //enviamos el mensaje de suscripcion al broker, revisamos si hay error y notificamos y salimos si es asi
        if (sendto(descriptor_socket, registro, strlen(registro), 0,
                   (SA*)&direccion_servidor, sizeof(direccion_servidor)) < 0) {
            printf("Error al suscribirse al partido %s\n", argv[i]);
            close(descriptor_socket);
            return 1;
        }
        printf("Suscrito al partido %s\n", argv[i]);
    }

    recibir_noticias(descriptor_socket);

    close(descriptor_socket);
    return 0;
}
