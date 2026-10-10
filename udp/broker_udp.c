#include <stdio.h> // entrada y salida estandar, printf()
#include <netinet/in.h> // estructuras para direcciones IP
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <strings.h> // bzero()
#include <sys/socket.h> // libreria principal para los sockets
#include <unistd.h> // read(), write(), close()
#define PORT 8080 // puerto del servidor
#define SA struct sockaddr  // alias para struct sockaddr
#define MAX_SUSCRIPTORES 50 // maximo numero de suscriptores registrados
#define MAX_BUFFER 1024 // maximo tamanio del buffer de mensajes
//numero maximo de partidos a los que puede estar suscrito un suscriptor
#define MAX_PARTIDOS 10

// Estructura que representa a cada suscriptor
struct Suscriptor {
    struct sockaddr_in direccion; //direccion (IP y puerto) desde la que se registro
    int activo; //1 si el cupo esta ocupado, 0 si esta libre

    //arreglo de strings que almacena los partidos de interes del suscriptor
    char partidos[MAX_PARTIDOS][50];

    int num_partidos; //numero de partidos de interes para el suscriptor
};

// Arreglo global de suscriptores
struct Suscriptor suscriptores[MAX_SUSCRIPTORES];

/*
 * Busca al suscriptor que tiene la direccion indicada.
 *
 * Retorna su posicion en el arreglo o -1 si no esta registrado.
 */
int buscar_suscriptor(struct sockaddr_in *origen)
{
    for (int i = 0; i < MAX_SUSCRIPTORES; i++) {
        if (suscriptores[i].activo &&
            suscriptores[i].direccion.sin_addr.s_addr == origen->sin_addr.s_addr &&
            suscriptores[i].direccion.sin_port == origen->sin_port)
            return i;
    }
    return -1;
}

/*
 * Indica si el suscriptor i esta suscrito al partido.
 *
 * Retorna 1 si lo esta y 0 si no.
 */
int esta_suscrito(int i, char *partido)
{
    //recorremos los partidos del suscriptor y comparamos con strcmp para revisar si ya esta suscrito al partido
    for (int k = 0; k < suscriptores[i].num_partidos; k++) {
        if (strcmp(suscriptores[i].partidos[k], partido) == 0)
            return 1;
    }
    return 0;
}

/*
 * Registra el partido de una linea SUBSCRIBER|PARTIDO
 * para el suscriptor que la envio desde la direccion origen.
 * Si el suscriptor ya existe, le agrega el partido.
 */
void registrar_suscriptor(struct sockaddr_in *origen, char *linea)
{
    //declaramos el arreglo de caracteres para almacenar el partido extraido del mensaje
    char partido[50];

    //usamos la funcion sscanf de la libreria stdio.h para extraer el partido del mensaje
    if (sscanf(linea + 11, "%49[^|]", partido) != 1) {
        //el mensaje no trae un partido, el registro es invalido
        printf("Registro invalido: %s\n", linea);
        return;
    }

    int i = buscar_suscriptor(origen);

    if (i == -1) {
        //es un suscriptor nuevo, buscamos el primer cupo libre
        for (i = 0; i < MAX_SUSCRIPTORES; i++) {
            if (!suscriptores[i].activo)
                break;
        }
        if (i == MAX_SUSCRIPTORES) {
            printf("Broker lleno, registro rechazado\n");
            return;
        }
        suscriptores[i].direccion = *origen;
        suscriptores[i].activo = 1;
        suscriptores[i].num_partidos = 0;
        printf("Suscriptor %d registrado\n", i);
    }

    //revisamos que el suscriptor no este ya suscrito al partido
    if (esta_suscrito(i, partido))
        return;

    //revisamos si el suscriptor ya tiene el maximo de partidos suscritos
    if (suscriptores[i].num_partidos < MAX_PARTIDOS) {
        //tiene menos partidos que el maximo, agregamos el nuevo partido al arreglo de partidos del suscriptor y aumentamos el contador de partidos
        //almacenamos el partido usando la funcion strcpy de la libreria string.h
        // esta funcion recibe el destino y la cadena de caracteres a copiar, para luego copiar el contenido en el destino
        strcpy(suscriptores[i].partidos[suscriptores[i].num_partidos++], partido);
        printf("Suscriptor %d suscrito a %s\n", i, partido);
    }
    else {
        //el suscriptor ya tiene el maximo de partidos suscritos, no podemos agregar mas
        printf("Suscriptor %d: maximo de partidos alcanzado\n", i);
    }
}

/*
 * Reenvia la noticia (PARTIDO|texto), sin modificarla,
 * a los suscriptores del partido.
 */
void reenviar_noticia(int descriptor_socket, char *mensaje, int longitud)
{
    char partido[50];

    //revisamos que el partido este en el formato esperado, que es "PARTIDO|MENSAJE"
    if (sscanf(mensaje, "%49[^|]", partido) != 1)
        return;

    //contador de suscriptores a los que se reenvio el mensaje
    int enviados = 0;

    for (int j = 0; j < MAX_SUSCRIPTORES; j++) {
        if (suscriptores[j].activo && esta_suscrito(j, partido)) {
            sendto(descriptor_socket, mensaje, longitud, 0,
                   (SA*)&suscriptores[j].direccion, sizeof(suscriptores[j].direccion));
            enviados++;
        }
    }
    //si nadie estaba suscrito al partido, la noticia se descarta (el broker no guarda noticias)
    if (enviados > 0)
        printf("Reenviado [%s] a %d suscriptores: %s\n", partido, enviados, mensaje);
    else
        printf("Sin suscriptores para [%s], mensaje descartado: %s\n", partido, mensaje);
}

/*
 * Crea el socket UDP del broker y recibe datagramas
 * en un bucle: registra suscriptores y reenvia noticias.
 */
int main()
{
    int descriptor_socket; // oido del socket del servidor

    // variables que almacenan las direcciones del servidor y del cliente
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto de la conexion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;
    struct sockaddr_in direccion_cliente;

    //inicializamos el arreglo de suscriptores
    for (int i = 0; i < MAX_SUSCRIPTORES; i++) {
        suscriptores[i].activo = 0;
        suscriptores[i].num_partidos = 0;
    }

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

    //activamos la opcion SO_REUSEADDR para el socket, asi cuando el broker se cierra y se
    //vuelve a abrir, el puerto no queda bloqueado
    int option_socket = 1;
    setsockopt(descriptor_socket, SOL_SOCKET, SO_REUSEADDR, &option_socket, sizeof(option_socket));

    //una vez creado el socket, usamos la funcion bzero de la libreria string.h para inicializar la
    //estructura de direccion del servidor en 0s
    bzero(&direccion_servidor, sizeof(direccion_servidor));

    //una vez creado el socket, replicamos su configuracion para la estructura
    //direccion_servidor,

    //declaramos la familia de direcciones
    direccion_servidor.sin_family = AF_INET;

    //asignamos la direccion IP del servidor
    //sin_addr es una estructura que contiene la direccion IP del servidor
    //s_addr es el campo de la estructura sin_addr que contiene la direccion IP en formato binario
    //INADDR_ANY indica que el servidor aceptara conexiones de cualquier direccion IP disponible en la maquina
    //htonl convierte la direccion IP de formato de host a formato de red
    direccion_servidor.sin_addr.s_addr = htonl(INADDR_ANY);

    //asignamos el puerto del servidor
    //htons convierte el puerto de formato de host a formato de red
    direccion_servidor.sin_port = htons(PORT);

    //el siguiente paso es hacer asociar el socket con la direccion IP y el puerto del servidor
    //para esto utilizamos la funcion bind de la libreria sys/socket.h
    //bind recibe tres parametros:
    // el descriptor del socket, para saber que socket asociar
    // un puntero a la direccion que se quiere asociar (direccion_servidor)
    // el tamaño de la estructura de direccion (sizeof(direccion_servidor))
    //    necesario dado que la estructura se recibe por puntero y el tamanio permite leer correctamente
    //si la asociacion fue exitosa, bind devuelve 0, si hubo un error devuelve -1
    if ((bind(descriptor_socket, (SA*)&direccion_servidor, sizeof(direccion_servidor))) == 0) {
        printf("Socket asociado correctamente..\n");
    }

    else {
        printf("Error al asociar el socket\n");
        exit(0);
    }

    printf("Broker UDP esperando datagramas..\n");

    char buffer[MAX_BUFFER];

    for (;;) {
        socklen_t longitud_direccion_cliente = sizeof(direccion_cliente);

        int n = recvfrom(descriptor_socket, buffer, MAX_BUFFER - 1, 0,
                         (SA*)&direccion_cliente, &longitud_direccion_cliente);
        if (n < 0) {
            printf("Error al recibir el datagrama\n");
            continue;
        }

        //agregamos un terminador nulo al final del buffer para saber hasta donde llega el mensaje recibido
        buffer[n] = '\0';

        if (strncmp(buffer, "SUBSCRIBER|", 11) == 0) {
            registrar_suscriptor(&direccion_cliente, buffer);
        }
        else if (strncmp(buffer, "PUBLISHER|", 10) == 0) {
            printf("Publisher registrado: %s\n", buffer + 10);
        }
        else {
            reenviar_noticia(descriptor_socket, buffer, n);
        }
    }
}
