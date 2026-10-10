#include <stdio.h> // entrada y salida estandar, printf()
#include <netinet/in.h> // estructuras para direcciones IP
#include <stdlib.h> // utilidades generales de C
#include <string.h>  // funciones para manipular cadenas
#include <sys/socket.h> // libreria principal para los sockets
#include <unistd.h> // read(), write(), close()
#include <signal.h> //ayuda a manejar señales, como SIGPIPE
#include <sys/select.h> //permite manejar multiples sockets a la vez
#define PORT 8080 // puerto del servidor
#define SA struct sockaddr  // alias para struct sockaddr
#define MAX_CLIENTES 50 // maximo numero de clientes conectados
#define MAX_BUFFER 1024 // maximo tamanio del buffer de mensajes
// Etiquetas para el tipo de cliente
#define SIN_REGISTRAR 0 
#define SUBSCRIBER 1
#define PUBLISHER 2
//numero maximo de partidos a los que puede estar suscrito un cliente
#define MAX_PARTIDOS 10

// Estructura que representa a cada cliente
struct Cliente {
    int socket; //socket asociado
    int tipo; //tipo de cliente

    //arreglo de strings que almacena los partidos de interes para los suscriptores
    char partidos[MAX_PARTIDOS][50]; 

    int num_partidos; //numero de partidos de interes para el cliente
    char buffer[MAX_BUFFER]; //buffer para almacenar datos recibidos del cliente
    int longitud; //bytes actualmente almacenados en el buffer
};

// Arreglo global de clientes
struct Cliente clientes[MAX_CLIENTES];

/*
 * Cierra el socket del cliente i y deja
 * su cupo libre en el arreglo de clientes.
 */
void liberar_cliente(int i)
{
    //libera el socket del cliente y lo marca como desconectado en el arreglo de clientes
    close(clientes[i].socket);
    clientes[i].socket = -1;
    clientes[i].tipo = SIN_REGISTRAR;
    clientes[i].num_partidos = 0;
    clientes[i].longitud = 0;
    printf("Cliente %d desconectado\n", i);
}

/*
 * Indica si el cliente i esta suscrito al partido.
 *
 * Retorna 1 si lo esta y 0 si no.
 */
int esta_suscrito(int i, char *partido)
{
    //recorremos los partidos del cliente y comparamos con strcmp para revisar si ya esta suscrito al partido
    for (int k = 0; k < clientes[i].num_partidos; k++) {
        if (strcmp(clientes[i].partidos[k], partido) == 0)
            return 1;
    }
    return 0;
}

/*
 * Procesa una linea completa recibida del cliente i.
 *
 * Segun el tipo de cliente:
 *
 * - Sin registrar: registra al cliente
 *   (SUBSCRIBER|PARTIDO1 o PUBLISHER).
 * - Suscriptor: agrega un partido adicional
 *   (SUBSCRIBER|PARTIDO2).
 * - Publicador: reenvia la noticia
 *   (PARTIDO1|texto) a los suscriptores del partido.
 */
void procesar_linea(int i, char *linea)
{
    //declaramos el arreglo de caracteres para almacenar el partido extraido del mensaje
    char partido[50];

    //procesamiento para clientes que todavia no se han registrado como suscriptores o publicadores
    if (clientes[i].tipo == SIN_REGISTRAR) {
        //para registrar un cliente se usa el siguiente formato de mensaje:
        // Suscriptores: "SUBSCRIBER|PARTIDO1" (para suscribirse a un partido)
        // Publicadores: "PUBLISHER" (para registrarse como publicador)
        //para determinar cual es usamos la funcion strncmp de la libreria string.h para comparar 
        //el inicio de la linea con lo esperado para cada tipo de cliente

        if (strncmp(linea, "SUBSCRIBER|", 11) == 0) {
            //es suscriptor
            //usamos la funcion sscanf de la libreria stdio.h para extraer el partido del mensaje
            if (sscanf(linea + 11, "%49[^|]", partido) == 1) {
                //el cliente solo se registra como suscriptor si el partido se pudo extraer del mensaje
                clientes[i].tipo = SUBSCRIBER;
                //almacenamos el partido en el arreglo de partidos del cliente usando la funcion strcpy de la libreria string.h
                // esta funcion recibe el destino y la cadena de caracteres a copiar, para luego copiar el contenido en el destino
                strcpy(clientes[i].partidos[0], partido);
                //inicializamos el numero de partidos del cliente en 1
                clientes[i].num_partidos = 1;
                printf("Cliente %d registrado como SUBSCRIBER de %s\n", i, partido);
            }
            else {
                //el mensaje no trae un partido, el registro es invalido y el cliente sigue sin registrar
                printf("Cliente %d: registro invalido: %s\n", i, linea);
            }
        }
        else if (strcmp(linea, "PUBLISHER") == 0) {
            //es publicador
            clientes[i].tipo = PUBLISHER;
            printf("Cliente %d registrado como PUBLISHER\n", i);
        }
        else {
            //si no es ninguno de los dos, el mensaje es invalido y no se registra al cliente
            printf("Cliente %d: registro invalido: %s\n", i, linea);
        }
        return;
    }

    //procesamiento para suscriptores que quieren suscribirse a un partido adicional
    //suscribirse a un partido adicional se hace con el mismo formato del registro inicial, solo detectamos que ya estaba registrado
    if (clientes[i].tipo == SUBSCRIBER && strncmp(linea, "SUBSCRIBER|", 11) == 0) {
        //extraemos el partido del mensaje y verificamos que no este ya suscrito usando la funcion esta_suscrito
        if (sscanf(linea + 11, "%49[^|]", partido) == 1 && !esta_suscrito(i, partido)) {
            //revisamos si el cliente ya tiene el maximo de partidos suscritos
            if (clientes[i].num_partidos < MAX_PARTIDOS) {
                //tiene menos partidos que el maximo, agregamos el nuevo partido al arreglo de partidos del cliente y aumentamos el contador de partidos
                strcpy(clientes[i].partidos[clientes[i].num_partidos++], partido);
                printf("Cliente %d suscrito tambien a %s\n", i, partido);
            }
            else {
                //el cliente ya tiene el maximo de partidos suscritos, no podemos agregar mas
                printf("Cliente %d: maximo de partidos alcanzado\n", i);
            }
        }
        return;
    }

    

    //procesamiento para publicadores que envian mensajes
    if (clientes[i].tipo == PUBLISHER) {
        //revisamos que el partido este en el formato esperado, que es "PARTIDO|MENSAJE"
        if (sscanf(linea, "%49[^|]", partido) != 1)
            return;

        //declaramos un arreglo de caracteres para almacenar el mensaje a reenviar a los suscriptores
        char mensaje[MAX_BUFFER + 1];

        //usamos la funcion snprintf de la libreria stdio.h para formatear el mensaje a reenviar (sin pasarnos del tamanio del buffer)
        //snprintf nos permite copiar el contenido de la linea en mensaje y agregar un salto de linea al final
        //la funcion recibe cuatro parametros:
        // un puntero al buffer donde se almacenara el mensaje formateado (mensaje)
        // el tamanio del mensaje (sizeof(mensaje))
        // la cadena de formato que indica como formatear el mensaje ("%s\n")
        // la cadena de caracteres a formatear (linea)
        //el retorno es el numero de caracteres que se habrian escrito en el buffer.
        int n = snprintf(mensaje, sizeof(mensaje), "%s\n", linea);

        //contador de suscriptores a los que se reenvio el mensaje
        int enviados = 0;

        //recorremos los clientes y reenviamos el mensaje a los que esten suscritos al partido indicado
        for (int j = 0; j < MAX_CLIENTES; j++) {
            //revisamos que este activo, sea suscriptor y este suscrito al partido
            if (clientes[j].socket != -1 &&
                clientes[j].tipo == SUBSCRIBER &&
                esta_suscrito(j, partido)) {
                //usamos la funcion send de la libreria sys/socket.h para enviar el mensaje al cliente
                //esta funcion envia datos a un socket y devuelve el numero de bytes enviados (-1 si error)
                //la funcion recibe cuatro parametros:
                // el descriptor del socket del cliente, para saber a que socket enviar
                // un puntero al buffer que contiene los datos a enviar (mensaje)
                // el tamanio del buffer (n)
                // un entero que indica las opciones de la funcion (0 para comportamiento por defecto)
                send(clientes[j].socket, mensaje, n, 0);
                enviados++;
            }
        }
        //si nadie estaba suscrito al partido, la noticia se descarta (el broker no guarda noticias)
        if (enviados > 0)
            printf("Reenviado [%s] a %d suscriptores: %s\n", partido, enviados, linea);
        else
            printf("Sin suscriptores para [%s], mensaje descartado: %s\n", partido, linea);
    }
}

/*
 * Lee los datos que llegaron del cliente y los
 * acumula en su buffer. Cada linea completa
 * (terminada en \n) se entrega a procesar_linea.
 * Si el cliente se desconecta, se libera su cupo.
 */
void leer_cliente(int num_cliente)
{
    //accedemos al cliente que queremos leer
    struct Cliente *cliente = &clientes[num_cliente];

    //para leer del socket del cliente usamos la funcion recv de la libreria sys/socket.h
    //esta funcion extrae datos del socket y los almacena en un buffer, y devuelve el numero de bytes leidos (-1 si error)
    //la funcion recibe cuatro parametros:
    // el descriptor del socket del cliente, para saber de que socket leer
    // un puntero al buffer donde se almacenaran los datos leidos (cliente->buffer + cliente->longitud apunta al primer byte libre del buffer)
    // el espacio restante en el buffer (MAX_BUFFER - cliente->longitud - 1) - evita sobrepasar el limite del buffer
    // un entero que indica las opciones de la funcion (0 para comportamiento por defecto)
    int n = recv(cliente->socket, cliente->buffer + cliente->longitud, MAX_BUFFER - cliente->longitud - 1, 0);

    //si el resultado de recv es 0 o negativo, el cliente se desconecto o hubo un error, liberamos su socket
    if (n <= 0) {
        liberar_cliente(num_cliente);
        return;
    }

    //redefinimos la longitud del buffer

    cliente->longitud += n;
    //agregamos un terminador nulo al final del buffer para saber hasta donde llega el mensaje recibido
    cliente->buffer[cliente->longitud] = '\0';


    //ahora queremos extraer las lineas completas del buffer, una linea completa se termina con \n
    //fin sera un puntero que apunta al primer \n encontrado en el buffer, si no hay \n, fin sera NULL
    char *fin;
    //para encontrar el primer \n en el buffer usamos la funcion strchr de la libreria string.h
    //mientras haya un \n en el buffer (fin != NULL), hay lineas completas que queremos procesar
    while ((fin = strchr(cliente->buffer, '\n')) != NULL) {
        //reemplazamos el \n por un terminador nulo para que la linea sea una cadena de caracteres valida
        *fin = '\0';
        //procesamos la linea completa que esta en cliente->buffer
        procesar_linea(num_cliente, cliente->buffer);

        //calculamos cuantos bytes quedan en el buffer despues de la linea procesada
        int restante = cliente->longitud - (int)(fin + 1 - cliente->buffer);
        //usamos la funcion memmove para mover los bytes restantes al inicio del buffer, sobrescribiendo la linea procesada
        //la funcion recibe tres parametros:
        // un puntero al destino (cliente->buffer)
        // un puntero al origen (fin + 1, que apunta al primer byte despues del \n)
        // el numero de bytes a mover (restante)
        memmove(cliente->buffer, fin + 1, restante);
        //actualizamos la longitud del buffer y agregamos un terminador nulo al final
        cliente->longitud = restante;
        cliente->buffer[cliente->longitud] = '\0';
    }

    //si el buffer del cliente esta lleno y no hay un \n, significa que el mensaje es demasiado largo, liberamos al cliente
    if (cliente->longitud >= MAX_BUFFER - 1) {
        printf("Cliente %d: mensaje demasiado largo\n", num_cliente);
        liberar_cliente(num_cliente);
    }
}

/*
 * Crea el socket del broker y, con select,
 * acepta conexiones nuevas y atiende a los
 * clientes ya conectados en un solo hilo.
 */
int main()
{
    int descriptor_socket; // oido del socket del servidor
    int descriptor_conexion; // descriptor para cada cliente/conexion entrante

    // variable que almacena la longitud de direccion del socket del cliente
    socklen_t longitud_direccion_cliente;

    //estructura de la libreria sys/select.h que indica los descriptores de sockets que queremos vigilar en el broker
    fd_set conjunto_lectura;

    //indica el mayor numero de descriptor activo en el broker
    int max_descriptor_activo;
    

    // variables que almacenan las direcciones del servidor y del cliente
    // se utiliza la estructura sockaddr_in, especifica para direcciones IPv4 de la libreria netinet/in.h
    //esta estructura contiene los siguientes campos:
    // sin_family: familia de direcciones, AF_INET para IPv4
    // sin_port: puerto de la conexion
    // sin_addr: direccion IP del host
    struct sockaddr_in direccion_servidor;
    struct sockaddr_in direccion_cliente;




    //signal es una funcion de la libreria signal.h que evita que el broker deje
    //de funcionar cuando reciba una señal SIGPIPE. Se declara al comienzo y es valido durante toda la ejecucion.
    //una señal SIGPIPE se genera cuando el broker intenta escribir en un socket que
    //ha sido cerrado por el cliente y que el broker no ha detectado.
    signal(SIGPIPE, SIG_IGN);


    //inicializamos el arreglo de clientes
    for (int i = 0; i < MAX_CLIENTES; i++) {
        clientes[i].socket = -1;
        clientes[i].tipo = SIN_REGISTRAR;
        clientes[i].num_partidos = 0;
        clientes[i].longitud = 0;
    }

    // crear el socket
    //usamos la funcion socket de la libreria sys/socket.h
    //esta funcion le pide al sistema operativo que cree un socket
    //y nos devuelve un descriptor que representa el socket, pero todavia no esta conectado
    //a ningun cliente. Los parametros usados cumplen la siguiente funcion:
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

    //una vez asociado el socket, esta listo para empezar a escuchar conexiones entrantes
    //para esto utilizamos la funcion listen de la libreria sys/socket.h
    //listen recibe dos parametros:
    // el descriptor del socket, para saber que socket escuchar
    // un entero que indica el tamaño de la cola de conexiones pendientes (escuchados pero no aceptados todavia)
    //si listen devuelve 0, significa que el socket esta escuchando correctamente
    if ((listen(descriptor_socket, 5)) == 0) {
        printf("Servidor escuchando..\n");
    }
    else {
        printf("Error al escuchar el socket\n");
        exit(0);
    }

    //ciclo infinito para aceptar conexiones y atender clientes ya conectados
    for (;;) {
        //fd_zero (libreria sys/select.h) permite inicializar el conjunto de descriptores de archivo (lectura) a cero
        //es necesario para eliminar la basura de la primera iteracion y, de la segunda en adelante, actualizar correctamente 
        //el conjunto de descriptores de archivo que se van a monitorear/leer
        FD_ZERO(&conjunto_lectura);

        //agregamos el descriptor del socket del servidor al conjunto de descriptores de archivo
        FD_SET(descriptor_socket, &conjunto_lectura);

        //inicializamos el maximo descriptor activo con el descriptor del socket del servidor
        max_descriptor_activo = descriptor_socket;

        //revisamos el arreglo de clientes y agregamos los descriptores de los que esten conectados al conjunto de descriptores de archivo
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].socket != -1) {
                FD_SET(clientes[i].socket, &conjunto_lectura);
                if (clientes[i].socket > max_descriptor_activo)
                    max_descriptor_activo = clientes[i].socket;
            }
        }


        //la funcion select de la libreria sys/select.h nos permite monitorear varios descriptores a la vez sin bloquear el programa esperando un solo descriptor
        //la funcion espera a que haya actividad en alguno de los descriptores del conjunto de lectura y luego marca aquellos con actividad con 
        //1 en el conjunto de lectura, mientras que los demas quedan en 0
        //la funcion recibe cinco parametros:
        // el mayor descriptor activo + 1 (max_descriptor_activo + 1)
        // un puntero al conjunto de descriptores de lectura (conjunto_lectura)
        // un puntero al conjunto de descriptores de escritura (NULL, no nos interesa)
        // un puntero al conjunto de descriptores de excepcion (NULL, no nos interesa)
        // un puntero a un struct timeval que indica el tiempo de espera (NULL, esperamos indefinidamente)
        //si select devuelve -1, significa que hubo un error
        if (select(max_descriptor_activo + 1, &conjunto_lectura, NULL, NULL, NULL) < 0) {
            printf("Error en select\n");
            continue;
        }

        //revisamos si el socket del broker tiene actividad (esta en el conjunto de lectura)
        //si es asi, significa que hay una nueva conexion entrante

        if (FD_ISSET(descriptor_socket, &conjunto_lectura)) {
            longitud_direccion_cliente = sizeof(direccion_cliente);

            //el socket del servidor esta escuchando y select nos indica que hay una nueva conexion entrante
            //para aceptarla utilizamos la funcion accept de la libreria sys/socket.h
            //accept recibe tres parametros:
            // el descriptor del socket, para saber que socket esta aceptando la conexion
            // un puntero a la estructura de direccion del cliente (direccion_cliente)
            // un puntero a un socklen_t que indica el tamaño de longitud_direccion_cliente
            //accept devuelve un descriptor de socket para la conexion aceptada, o -1 si hubo un error

            descriptor_conexion = accept(descriptor_socket, (SA*)&direccion_cliente, &longitud_direccion_cliente);

            if (descriptor_conexion == -1) {
                printf("Error al aceptar la conexion\n");
            }
            else {
                printf("Conexion aceptada correctamente..\n");

            //una vez aceptada la conexion, buscamos un cupo en el arreglo de clientes para almacenar al cliente entrante
                int i;
                for (i = 0; i < MAX_CLIENTES; i++) {
                    if (clientes[i].socket == -1) {
                        //buscamos el primer cupo libre y lo usamos, inicializando al cliente con tipo sin_registrar
                        clientes[i].socket = descriptor_conexion;
                        clientes[i].tipo = SIN_REGISTRAR;
                        clientes[i].num_partidos = 0;
                        clientes[i].longitud = 0;
                        printf("Cliente asignado al slot %d\n", i);
                        break;
                    }
                }
                //si el arreglo de clientes esta lleno, rechazamos la conexion y cerramos el socket
                if (i == MAX_CLIENTES) {
                    printf("Broker lleno, conexion rechazada\n");
                    close(descriptor_conexion);
                }
            }
        }

        //revisamos el arreglo de clientes para leer los que tienen actividad
        //para cada uno, leemos el mensaje que haya enviado y lo procesamos con leer_cliente(i)
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (clientes[i].socket != -1 && FD_ISSET(clientes[i].socket, &conjunto_lectura))
                leer_cliente(i);
        }
    }
}
