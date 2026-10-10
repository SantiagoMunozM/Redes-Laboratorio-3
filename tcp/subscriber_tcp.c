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
#define MAX_BUFFER 1024
#define MAX_PARTIDOS 10

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
        // el descriptor del socket del subscriber conectado al broker, para saber a que socket enviar
        // un puntero al buffer que contiene los datos a enviar (linea+enviados, primer byte no enviado)
        // el tamanio del buffer (n - enviados)
        // un entero que indica las opciones de la funcion (0 para comportamiento por defecto)
        int r = send(descriptor_socket, linea + enviados, n - enviados, 0);

        //el retorno de send es el numero de bytes que se enviaron, si es negativo hubo un error y retornamos -1
        if (r < 0)
            return -1;

        //si no fue negativo agregamos los bytes enviados al contador
        enviados += r;
    }
    return 0;
}

/*
 * Recibe las noticias que reenvia el broker y las
 * imprime, una por linea. Termina cuando el broker
 * cierra la conexion.
 */
void recibir_noticias(int descriptor_socket)
{
    char buffer[MAX_BUFFER];
    int longitud = 0;

    for (;;) {
        //para leer del socket usamos la funcion recv de la libreria sys/socket.h
        //esta funcion extrae datos del socket y los almacena en un buffer, y devuelve el numero de bytes leidos (-1 si error)
        //la funcion recibe cuatro parametros:
        // el descriptor del socket del subscriber conectado al broker, para saber de que socket leer
        // un puntero al buffer donde se almacenaran los datos leidos (buffer + longitud apunta al primer byte libre del buffer)
        // el espacio restante en el buffer (MAX_BUFFER - longitud - 1) - evita sobrepasar el limite del buffer
        // un entero que indica las opciones de la funcion (0 para comportamiento por defecto)
        int n = recv(descriptor_socket, buffer + longitud, MAX_BUFFER - longitud - 1, 0);
        //si el resultado de recv es 0 o negativo, el broker cerro la conexion o hubo un error, salimos de la funcion
        if (n <= 0) {
            printf("El broker cerro la conexion\n");
            return;
        }

        //agregamos los bytes leidos a la longitud del buffer y agregamos un terminador nulo al final
        longitud += n;
        buffer[longitud] = '\0';

        //ahora queremos extraer las lineas completas del buffer, una linea completa se termina con \n
        //fin sera un puntero que apunta al primer \n encontrado en el buffer, si no hay \n, fin sera NULL
        char *fin;
        //para encontrar el primer \n en el buffer usamos la funcion strchr de la libreria string.h
        //mientras haya un \n en el buffer (fin != NULL), hay lineas completas que queremos procesar     
        while ((fin = strchr(buffer, '\n')) != NULL) {
            //reemplazamos el \n por un terminador nulo para que la linea sea una cadena de caracteres valida
            *fin = '\0';
            printf("Noticia recibida: %s\n", buffer);

            //calculamos cuantos bytes quedan en el buffer despues de la linea procesada
            int restante = longitud - (int)(fin + 1 - buffer);
            //usamos la funcion memmove para mover los bytes restantes al inicio del buffer, sobrescribiendo la linea procesada
            //la funcion recibe tres parametros:
                // un puntero al destino (buffer)
                // un puntero al origen (fin + 1, que apunta al primer byte despues del \n)
                // el numero de bytes a mover (restante)
            memmove(buffer, fin + 1, restante);
            //actualizamos la longitud del buffer y agregamos un terminador nulo al final
            longitud = restante;
            buffer[longitud] = '\0';
        }
        //si el buffer del cliente esta lleno y no hay un \n el mensaje es demasiado largo, notificamos y terminamos el programa
        if (longitud >= MAX_BUFFER - 1) {
            printf("Mensaje demasiado largo\n");
            return;
        }
    }
}

/*
 * Se conecta al broker, se suscribe a los partidos
 * indicados y muestra las noticias que le reenvia.
 *
 * Uso: ./subscriber_tcp <partido1> [partido2 ...]
 */
int main(int argc, char *argv[])
{
    //verificamos uso correcto de argumentos
    if (argc < 2 || argc - 1 > MAX_PARTIDOS) {
        printf("Uso: %s <partido1> [partido2 ...] (maximo %d partidos)\n", argv[0], MAX_PARTIDOS);
        printf("Ejemplo: %s ColombiaVsBrasil ArgentinaVsChile\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        //usando la funcion strchr de la libreria string.h rechazamos si el partido contiene un '|'
        //usando la funcion strlen de la libreria string.h rechazamos si el partido tiene mas de 49 caracteres o es vacio
        if (strchr(argv[i], '|') != NULL || strlen(argv[i]) > 49 || strlen(argv[i]) == 0) {
            printf("Partido invalido: no puede tener '|' ni mas de 49 caracteres\n");
            return 1;
        }
    }

    int descriptor_socket; // descriptor del socket del cliente

    // variable que almacena la direccion del servidor al que nos conectaremos
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto de la conexion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;

    //signal es una funcion de la libreria signal.h que evita que el subscriber deje
    //de funcionar cuando reciba una señal SIGPIPE. Se declara al comienzo y es valido durante toda la ejecucion.
    //una señal SIGPIPE se genera cuando el subscriber intenta escribir en un socket que
    //ha sido cerrado por el servidor y que el subscriber no ha detectado.
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
    //inet_addr convierte la direccion IP escrita como texto ("127.0.0.1", localhost) a formato binario
    direccion_servidor.sin_addr.s_addr = inet_addr("127.0.0.1");

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
    
    //revisa los partidos indicados en los argumentos, envia un mensaje de suscripcion al broker por cada uno
    for (int i = 1; i < argc; i++) {
        char registro[MAX_MENSAJE];

        //usamos la funcion snprintf de la libreria stdio.h para formatear el mensaje de suscripcion al broker
        //la funcion nos permite copiar el contenido del i-esimo partido en el buffer y agregarle el prefijo de reconocimiento de suscriptor
        //asi, el mensaje tiene el formato correcto para el broker
        snprintf(registro, sizeof(registro), "SUBSCRIBER|%s", argv[i]);

        //enviamos el mensaje de suscripcion al broker, revisamos si hay error y notificamos y salimos si es asi
        if (enviar_linea(descriptor_socket, registro) < 0) {
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
