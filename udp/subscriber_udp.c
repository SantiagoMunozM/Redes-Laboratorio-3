#include <stdio.h> // entrada y salida estandar, printf()
#include <arpa/inet.h> // inet_addr(), htons()
#include <netdb.h> // definiciones para operaciones de red
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <strings.h> // bzero()
#include <sys/socket.h> // libreria principal para los sockets
#include <unistd.h> // read(), write(), close()
#define PORT 8080 // puerto del broker al que se envian los registros
#define SA struct sockaddr  // alias para struct sockaddr
#define MAX_MENSAJE 512 // maximo tamanio del mensaje de registro a enviar al broker
#define MAX_BUFFER 1024 // maximo tamanio del buffer de noticias recibidas
#define MAX_PARTIDOS 10 // maximo numero de partidos a los que puede suscribirse el suscriptor

/*
 * Recibe las noticias que reenvia el broker y las
 * imprime, una por datagrama. No termina por si sola:
 * se detiene con Ctrl+C.
 */
void recibir_noticias(int descriptor_socket)
{
    //declaramos el buffer de recepcion de noticias
    char buffer[MAX_BUFFER];

    //bucle infinito para recibir noticias de forma continua
    for (;;) {
        //recibimos las noticias mediante la funcion recvfrom de la libreria sys/socket.h. La funcion espera
        //que llegue un datagrama al socket, lo copia en el buffer y devuelve la cantidad de bytes recibidos.
        //En el proceso, bloquea el programa hasta que llegue un datagrama
        //esta funcion recibe los siguientes parametros:
        // descriptor del socket, para saber de que socket recibir el datagrama (descriptor_socket)
        // puntero al buffer donde se almacenara el datagrama recibido (buffer)
        // tamanio del buffer (MAX_BUFFER - 1), dejando un byte libre para el terminador nulo
        // flags, que en este caso es 0, indicando que no se usan banderas
        // puntero a la estructura donde se guardaria la direccion de quien envio el datagrama (NULL)
        // puntero a la longitud de esa estructura (NULL)
        //los ultimos dos parametros son NULL porque al suscriptor no le interesa quien envio el datagrama,
        //solo necesita el contenido de la noticia (el broker es el unico que le escribe)
        int n = recvfrom(descriptor_socket, buffer, MAX_BUFFER - 1, 0, NULL, NULL);

        //si el valor retornado por recvfrom es negativo, hubo error en la transferencia.
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
 * Uso: ./subscriber_udp <partido1> [partido2 ...]
 */
int main(int argc, char *argv[])
{
    //verificamos uso correcto de argumentos (tiene el numero correcto y no excede el limite de partidos)
    if (argc < 2 || argc - 1 > MAX_PARTIDOS) {
        printf("Uso: %s <partido1> [partido2 ...] (maximo %d partidos)\n", argv[0], MAX_PARTIDOS);
        printf("Ejemplo: %s ColombiaVsBrasil ArgentinaVsChile\n", argv[0]);
        return 1;
    }

    //ciclo para revisar los partidos ingresados por parametro, rechazando aquellos con mas de 49 caracteres, vacios o que contengan un '|'
    for (int i = 1; i < argc; i++) {
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
    // sin_port: puerto a usar en la direccion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;

    // crear el socket
    //usamos la funcion socket de la libreria sys/socket.h
    //esta funcion le pide al sistema operativo que cree un socket
    //y nos devuelve un descriptor que representa el socket.
    //Los parametros usados cumplen la siguiente funcion:
    // AF_INET: indica que se usara el protocolo IPv4 (familia de direcciones)
    // SOCK_DGRAM: indica que se usara un socket de tipo datagrama
    // 0: indica usar el protocolo por defecto para la combinacion de protocolo IP y tipo de socket.
    descriptor_socket = socket(AF_INET, SOCK_DGRAM, 0);

    //si el descriptor es -1, significa que hubo un error al crear el socket
    if (descriptor_socket == -1) {
        //si hubo error al crear el socket, terminamos el programa y mostramos mensaje de error
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
    //inet_addr convierte la direccion IP escrita como texto ("127.0.0.1", localhost) a formato binario
    direccion_servidor.sin_addr.s_addr = inet_addr("127.0.0.1");

    //asignamos el puerto del servidor
    //htons convierte el puerto de formato de host a formato de red
    direccion_servidor.sin_port = htons(PORT);

    //el suscriptor no usa bind: el sistema operativo le asigna un puerto libre automaticamente la primera
    //vez que se envia un datagrama con sendto. Ese mismo socket y puerto se usan despues para recibir las
    //noticias, por eso el broker, que guarda la direccion de origen del registro, sabe a donde reenviarlas

    //revisa los partidos indicados en los argumentos, envia un mensaje de suscripcion al broker por cada uno
    for (int i = 1; i < argc; i++) {
        char registro[MAX_MENSAJE];

        //usamos la funcion snprintf de la libreria stdio.h para formatear el mensaje de suscripcion al broker
        //la funcion nos permite copiar el contenido del i-esimo partido en el buffer y agregarle el prefijo de reconocimiento de suscriptor
        //asi, el mensaje tiene el formato correcto para el broker usando %s para insertar el partido en la cadena de caracteres
        snprintf(registro, sizeof(registro), "SUBSCRIBER|%s", argv[i]);

        //enviamos el mensaje de suscripcion al broker, revisamos si hay error y notificamos y salimos si es asi
        //para esto usamos la funcion sendto de la libreria sys/socket.h
        //la cual nos permite enviar un datagrama a una direccion IP y puerto ingresados por parametro
        //la funcion recibe los siguientes parametros:
        // descriptor del socket, para saber por que socket enviar el mensaje (descriptor_socket)
        // puntero al mensaje a enviar (registro)
        // longitud del mensaje a enviar, calculada con strlen de la libreria string.h (strlen(registro))
        // flags, que en este caso es 0, indicando que no se usan banderas
        // puntero a la direccion del broker al que se le enviara el mensaje (direccion_servidor)
        // tamanio de la estructura de direccion del broker (sizeof(direccion_servidor))
        //sendto retorna la cantidad de bytes enviados o -1 si hubo error. Que retorne con exito solo significa
        //que el sistema operativo acepto el datagrama, no que el broker lo haya recibido: si el registro se
        //pierde, el suscriptor no queda suscrito a ese partido y no recibira sus noticias (UDP no confirma la entrega)
        if (sendto(descriptor_socket, registro, strlen(registro), 0,
                   (SA*)&direccion_servidor, sizeof(direccion_servidor)) < 0) {
            printf("Error al suscribirse al partido %s\n", argv[i]);
            close(descriptor_socket);
            return 1;
        }
        printf("Suscrito al partido %s\n", argv[i]);
    }

    //una vez registrados los partidos, quedamos esperando las noticias que reenvie el broker
    recibir_noticias(descriptor_socket);

    //usando la funcion close de la libreria unistd.h cerramos el socket, liberando el descriptor y el puerto asignado
    //en la practica esta linea no se alcanza porque recibir_noticias nunca termina (el programa se detiene con Ctrl+C)
    close(descriptor_socket);
    return 0;
}
