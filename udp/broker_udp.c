#include <stdio.h> // entrada y salida estandar, printf()
#include <netinet/in.h> // estructuras para direcciones IP
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <strings.h> // bzero()
#include <sys/socket.h> // libreria principal para los sockets
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
    //recorremos el arreglo de suscriptores para encontrar al suscriptor con la direccion y puerto dados
    //ademas, verifica que el suscriptor este activo.
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
    //esta nos permite revisar si el texto del partido sigue el formato esperado (menos de 50 caracteres y sin el caracter '|')
    if (sscanf(linea + 11, "%49[^|]", partido) != 1) {
        //el mensaje no trae un partido, el registro es invalido
        printf("Registro invalido: %s\n", linea);
        return;
    }

    //buscamos si el suscriptor ya esta registrado
    int i = buscar_suscriptor(origen);

    if (i == -1) {
        //es un suscriptor nuevo, buscamos el primer cupo libre
        for (i = 0; i < MAX_SUSCRIPTORES; i++) {
            //si encontramos uno inactivo, lo usamos para el nuevo suscriptor
            if (!suscriptores[i].activo)
                break;
        }
        if (i == MAX_SUSCRIPTORES) {
            //si no hay cupos libres, rechaza el registro
            printf("Broker lleno, registro rechazado\n");
            return;
        }
        //registrar la informacion del nuevo suscriptor en su estructura
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

    //usamos la funcion sscanf de la libreria stdio.h para saber si el mensaje tiene el formato correcto para el envio (tiene por lo menos un caracter antes de |).
    if (sscanf(mensaje, "%49[^|]", partido) != 1)
        return;

    //contador de suscriptores a los que se reenvio el mensaje
    int enviados = 0;

    //recorremos todos los suscriptores
    for (int j = 0; j < MAX_SUSCRIPTORES; j++) {
        //enviamos solo a los suscriptores que esten activos y suscritos al partido
        if (suscriptores[j].activo && esta_suscrito(j, partido)) {

            //para enviar el mensaje, usamos la funcion sendto de la libreria sys/socket.h
            //la cual nos permite enviar un datagrama a una direccion IP y puerto ingresados por parametro
            //la funcion recibe los siguientes parametros:
            // descriptor del socket, para saber a que socket enviar el mensaje (descriptor_socket)
            // puntero al mensaje a enviar (mensaje)
            // longitud del mensaje a enviar (longitud)
            // flags, que en este caso es 0, indicando que no se usan banderas 
            // puntero a la direccion del suscriptor al que se le enviara el mensaje (posicion en el arreglo)
            // tamanio de la estructura de direccion del suscriptor (tamanio de la estructura en el arreglo)
            sendto(descriptor_socket, mensaje, longitud, 0,
                   (SA*)&suscriptores[j].direccion, sizeof(suscriptores[j].direccion));
            
            //incrementamos contador para saber a cuantos suscriptores se ha enviado el mensaje
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
    int descriptor_socket; // descriptor del servidor

    // variables que almacenan las direcciones del servidor y del cliente
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto
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
    // SOCK_DGRAM: indica que se usara un socket de tipo datagrama 
    // 0: indica usar el protocolo por defecto para la combinacion de protocolo IP y tipo de socket. (con datagramas e IPv4, UDP)
    descriptor_socket = socket(AF_INET, SOCK_DGRAM, 0);

    //si el descriptor es -1, significa que hubo un error al crear el socket
    if (descriptor_socket == -1) {
        //si hubo error al crear el socket, terminamos el programa y mostramos mensaje de error
        printf("Error al crear el socket\n");
        exit(0);
    }
    else
        printf("Socket creado correctamente..\n");

    //activamos la opcion SO_REUSEADDR para el socket, asi cuando el broker se cierra y se
    //vuelve a abrir, el puerto no queda bloqueado
    //para esto usamos la funcion setsockopt de la libreria sys/socket.h
    int option_socket = 1;
    setsockopt(descriptor_socket, SOL_SOCKET, SO_REUSEADDR, &option_socket, sizeof(option_socket));

    //una vez creado el socket, usamos la funcion bzero de la libreria strings.h para inicializar la
    //estructura de direccion del servidor en 0s
    bzero(&direccion_servidor, sizeof(direccion_servidor));

    //una vez creado el socket, replicamos su configuracion para la estructura de direccion_servidor

    //declaramos la familia de direcciones
    direccion_servidor.sin_family = AF_INET;

    //asignamos la direccion IP del servidor
    //sin_addr es una estructura que contiene la direccion IP del servidor
    //s_addr es el campo de la estructura sin_addr que contiene la direccion IP en formato binario
    //INADDR_ANY indica que el servidor aceptara datagramas de cualquier direccion IP disponible en la maquina
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
        //si hubo error al asociar, terminamos el programa y mostramos mensaje de error
        printf("Error al asociar el socket\n");
        exit(0);
    }

    printf("Broker UDP esperando datagramas..\n");

    //declaramos el buffer de recepcion de datagramas
    char buffer[MAX_BUFFER];

    //bucle infinito para recibir datagramas de forma continua
    for (;;) {
        //inicializamos la longitud de direccion del cliente
        socklen_t longitud_direccion_cliente = sizeof(direccion_cliente);

        //dada la simplicidad de UDP, podemos recibir datagramas de forma sencilla mediante la
        //funcion recvfrom de la libreria sys/socket.h. La funcion espera que llegue un datagrama 
        //al socket, lo copia en el buffer y devuelve la cantidad de bytes recibidos. En el proceso, bloquea el programa
        //hasta que llegue un datagrama
        //esta funcion recibe los siguientes parametros:
        // descriptor del socket, para saber de que socket recibir el datagrama (descriptor_socket)
        // puntero al buffer donde se almacenara el datagrama recibido (buffer)
        // tamanio del buffer (MAX_BUFFER - 1)
        // flags, que en este caso es 0, indicando que no se usan banderas
        // puntero a la estructura de direccion del cliente que envio el datagrama (direccion_cliente)
        // puntero a la longitud de la estructura de direccion del cliente (longitud_direccion_cliente, es modificado por la funcion con el tamanio real de la estructura)
        int n = recvfrom(descriptor_socket, buffer, MAX_BUFFER - 1, 0,
                         (SA*)&direccion_cliente, &longitud_direccion_cliente);

        //si el valor retornado por recvfrom es negativo, hubo error en la transferencia.
        if (n < 0) {
            printf("Error al recibir el datagrama\n");
            continue;
        }

        //agregamos un terminador nulo al final del buffer para saber hasta donde llega el mensaje recibido
        buffer[n] = '\0';
        //evaluamos el contenido del mensaje para saber si se trata de un registro de suscriptor, publicador o una noticia
        //usamos la funcion strncmp de la libreria string.h para comparar el inicio del mensaje con el formato para cada accion
        if (strncmp(buffer, "SUBSCRIBER|", 11) == 0) {
            //formato para registrar un suscriptor: SUBSCRIBER|PARTIDO
            registrar_suscriptor(&direccion_cliente, buffer);
        }
        else if (strncmp(buffer, "PUBLISHER|", 10) == 0) {
            //formato para registrar un publicador: PUBLISHER|NOMBRE
            printf("Publisher registrado: %s\n", buffer + 10);
        }
        else {
            //formato para enviar una noticia: PARTIDO|MENSAJE
            reenviar_noticia(descriptor_socket, buffer, n);
        }
    }
}
