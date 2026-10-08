# Bono: sistema Pub/Sub sobre QUIC (msquic)

Tercera versión del sistema de noticias deportivas. Es el mismo modelo broker / publicadores / suscriptores de las versiones TCP y UDP, pero el transporte es **QUIC**: un protocolo confiable y ordenado (como TCP) que corre **sobre UDP** y lleva **TLS 1.3 obligatorio**.

La API de sockets de C (`sys/socket.h`) no implementa QUIC, así que se usa la librería externa **msquic** de Microsoft (licencia MIT, API en C). Como pide el enunciado, la sección 6 documenta una por una las funciones de la librería que usa el programa.

| Archivo | Rol |
|---|---|
| `broker_quic.c` | Broker: acepta conexiones QUIC y reenvía cada noticia a los suscriptores de ese partido |
| `publisher_quic.c` | Publicador (periodista): envía eventos de un partido |
| `subscriber_quic.c` | Suscriptor (hincha): sigue uno o varios partidos |
| `instalar_msquic.sh` | Instala msquic, descarga sus headers y genera el certificado TLS |

---

## 1. Instalación (en **todas** las máquinas, una vez)

Requiere Ubuntu o Debian con `sudo` e internet (probado en Ubuntu 24.04 con libmsquic 2.6.2).

```bash
cd quic
chmod +x instalar_msquic.sh
./instalar_msquic.sh
```

El script:
1. Instala `gcc`, `openssl` y `curl`.
2. Agrega el repositorio apt de Microsoft (`packages.microsoft.com`) e instala el paquete `libmsquic`.
3. El paquete solo trae el binario `libmsquic.so.2`, sin los headers. Por eso el script descarga `msquic.h`, `msquic_posix.h` y `quic_sal_stub.h` de GitHub, **de la misma versión instalada**, a `quic/include/`.
4. Genera un certificado autofirmado (`server.cert` y `server.key`). Solo lo usa el broker.

Si se prefiere instalar a mano:
```bash
source /etc/os-release
curl -sSL -O https://packages.microsoft.com/config/$ID/$VERSION_ID/packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb && sudo apt-get update
sudo apt-get install -y libmsquic gcc openssl curl
V=$(dpkg-query -W -f='${Version}' libmsquic | cut -d- -f1); mkdir -p include
for f in msquic.h msquic_posix.h quic_sal_stub.h; do
  curl -sSfL -o include/$f https://raw.githubusercontent.com/microsoft/msquic/v$V/src/inc/$f; done
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.cert -days 365 -subj "/CN=broker"
```

## 2. Compilación (dentro de `quic/`)

```bash
gcc -Wall broker_quic.c     -o broker_quic     -Iinclude -l:libmsquic.so.2 -lpthread
gcc -Wall publisher_quic.c  -o publisher_quic  -Iinclude -l:libmsquic.so.2 -lpthread
gcc -Wall subscriber_quic.c -o subscriber_quic -Iinclude -l:libmsquic.so.2 -lpthread
```

| Flag | Para qué |
|---|---|
| `-Iinclude` | busca `msquic.h` en la carpeta `include/` que creó el script |
| `-l:libmsquic.so.2` | enlaza con la librería por su nombre exacto (el paquete no crea el enlace `libmsquic.so`, por eso `-lmsquic` no funciona) |
| `-lpthread` | mutex y variables de condición (los callbacks de msquic corren en hilos propios) |
| `-Wall` | muestra todas las advertencias |

Basta compilar en cada máquina el programa que va a correr en ella.

## 3. Ejecución

Siempre se inicia **primero el broker**, y luego los suscriptores antes que los publicadores (el broker no guarda noticias para quien se conecte tarde).

```bash
# Máquina del broker (en la carpeta donde están server.cert y server.key)
ip addr                        # ver la IP de la máquina, ej. 192.168.1.20
sudo ufw allow 7000/udp        # solo si el firewall está activo
./broker_quic                  # Enter para detenerlo
# opcional: ./broker_quic <certificado> <llave>

# Máquinas de los suscriptores: IP del broker y uno o más partidos (máximo 8)
./subscriber_quic 192.168.1.20 ColombiaVsBrasil RealVsBarca
./subscriber_quic 192.168.1.20 RealVsBarca          # Enter para salir

# Máquinas de los publicadores: IP del broker, partido y modo
./publisher_quic 192.168.1.20 ColombiaVsBrasil auto     # 10 eventos, 1 por segundo
./publisher_quic 192.168.1.20 RealVsBarca manual        # se escriben por teclado; "exit" termina
```

También funciona todo en una sola máquina usando `127.0.0.1` como IP del broker. Si se omite el modo del publicador, se usa `auto`.

Prueba mínima del enunciado: 1 broker, 2 suscriptores y 2 publicadores en modo `auto`, es decir, 10 mensajes por publicador.

## 4. Formato de los mensajes

Son mensajes de texto plano con campos separados por `|`, y **cada mensaje termina en `\n`**. Un stream QUIC es un flujo de bytes igual que TCP: un mensaje puede llegar partido o pegado con otro, así que el receptor acumula bytes y corta en cada `\n`.

| Dirección | Formato | Ejemplo |
|---|---|---|
| Suscriptor → broker (registro) | `SUBSCRIBER\|<partido1>,<partido2>,...` | `SUBSCRIBER\|ColombiaVsBrasil,RealVsBarca` |
| Broker → suscriptor (confirmación) | `OK\|<partido1>,<partido2>,...` | `OK\|ColombiaVsBrasil,RealVsBarca` |
| Publicador → broker (registro) | `PUBLISHER\|<partido>` | `PUBLISHER\|ColombiaVsBrasil` |
| Broker → publicador (confirmación) | `OK\|<partido>` | `OK\|ColombiaVsBrasil` |
| Publicador → broker (noticia) | `<partido>\|<texto>` | `ColombiaVsBrasil\|[03] Gol de Equipo A al minuto 32` |
| Broker → suscriptor (noticia) | la misma línea, **sin modificar** | `ColombiaVsBrasil\|[03] Gol de Equipo A al minuto 32` |

- Los nombres de partido no pueden tener `|`, `,` ni espacios, y miden máximo 49 caracteres.
- Un mensaje mide máximo 256 bytes; lo que sobre se descarta.
- En modo `auto`, el publicador agrega el número de evento `[01]`…`[10]` para verificar en el suscriptor que no falte ninguno y que lleguen en orden.
- El suscriptor muestra cada noticia así: `(n) [partido] texto`.

## 5. Diseño

- **Transporte:** UDP puerto **7000**. El ALPN `pubsub` es el nombre del protocolo de aplicación que se negocia en el handshake TLS. El broker presenta un certificado autofirmado y los clientes no lo validan (`QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION`). En un sistema real se validaría contra una autoridad certificadora.
- **Una conexión y un stream bidireccional por cliente.** Todo lo de un cliente viaja por ese stream, lo que garantiza el orden de los eventos de un partido. *Alternativa:* abrir un stream por mensaje evita el bloqueo de cabeza de línea (head-of-line blocking): si se pierde un paquete, los otros streams no esperan. A cambio, no hay orden entre streams, y un "2–1" podría mostrarse antes que el "1–1".
- **Concurrencia:** msquic atiende los eventos con *callbacks* en sus propios hilos. El broker protege la tabla de clientes (`struct Cliente clientes[50]`) con un `pthread_mutex_t`. En el broker TCP con `select()` no hace falta, porque corre en un solo hilo.
- **Detección de caídas:** `IdleTimeoutMs = 30000` y, en los clientes, `KeepAliveIntervalMs = 5000` (un PING cada 5 s). Medido en las pruebas:
  - Si un suscriptor muere con `kill -9`, el broker lo saca de la tabla en ~30 s.
  - Si el broker muere, el suscriptor lo detecta en menos de 30 s.
  - Si el broker se detiene con Enter, los clientes se enteran de inmediato ("El broker cerró la conexión").
- **Puerto ocupado:** msquic abre su socket con `SO_REUSEPORT`, así que un segundo broker en el puerto 7000 *no* daría error y el sistema operativo repartiría los clientes entre ambos. Para evitarlo, `broker_quic` primero prueba un `bind()` UDP normal (`puerto_ocupado()`) y termina si el puerto ya está en uso.
- **Envío asíncrono:** `StreamSend` no copia los datos. Cada mensaje se reserva con `malloc` y se libera en el evento `SEND_COMPLETE`.

### Equivalencias con la API de sockets

| Sockets TCP | msquic |
|---|---|
| `socket()` (servidor) | `ListenerOpen` |
| `bind()` + `listen()` | `ListenerStart` |
| `accept()` | evento `QUIC_LISTENER_EVENT_NEW_CONNECTION` |
| `socket()` (cliente) | `ConnectionOpen` |
| `connect()` | `ConnectionStart` (+ evento `CONNECTED`) |
| `send()` / `write()` | `StreamSend` |
| `recv()` / `read()` | evento `QUIC_STREAM_EVENT_RECEIVE` |
| `recv()` devuelve 0 (el otro cerró) | evento `PEER_SEND_SHUTDOWN` / `SHUTDOWN_INITIATED_BY_PEER` |
| `shutdown(SHUT_WR)` | `StreamShutdown(GRACEFUL)` |
| `close()` | `ConnectionShutdown` + `ConnectionClose` |

## 6. Documentación de las funciones de msquic usadas

Todas las funciones, salvo `MsQuicOpen2`/`MsQuicClose` y las de direcciones, se llaman a través de la tabla `const QUIC_API_TABLE* msquic`. Las que pueden fallar devuelven `QUIC_STATUS`, que se revisa con las macros `QUIC_FAILED(s)` / `QUIC_SUCCEEDED(s)`. En Linux, un `QUIC_STATUS` es un código `errno`, por eso los programas lo muestran con `strerror()`.

### Inicialización (broker, publicador y suscriptor)

| Función | Parámetros | Qué hace en el programa |
|---|---|---|
| `MsQuicOpen2(&msquic)` | puntero donde dejar la tabla | Carga la librería y llena la tabla de funciones (API versión 2). Es lo primero que se llama. |
| `RegistrationOpen(&config, &registro)` | `QUIC_REGISTRATION_CONFIG` {nombre de la app, perfil `QUIC_EXECUTION_PROFILE_LOW_LATENCY`}; salida: handle | Crea el *registro*, el contexto de ejecución de la app (hilos de trabajo de msquic). |
| `ConfigurationOpen(registro, &alpn, 1, &settings, sizeof(settings), NULL, &config)` | registro, lista de ALPN y cantidad, `QUIC_SETTINGS` y su tamaño, contexto, salida | Crea la configuración de las conexiones. Settings usados: `IdleTimeoutMs`, `KeepAliveIntervalMs` (clientes) y `PeerBidiStreamCount = 1` (broker: cada cliente puede abrir 1 stream). Solo se aplican los campos marcados en `IsSet`. |
| `ConfigurationLoadCredential(config, &cred)` | configuración, `QUIC_CREDENTIAL_CONFIG` | Carga las credenciales TLS 1.3. **Broker:** `Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE` con las rutas de `server.cert` y `server.key`. **Clientes:** `Type = QUIC_CREDENTIAL_TYPE_NONE`, `Flags = CLIENT \| NO_CERTIFICATE_VALIDATION`. |

### Broker

| Función | Parámetros | Qué hace en el programa |
|---|---|---|
| `ListenerOpen(registro, callback_listener, NULL, &listener)` | registro, función callback, contexto, salida | Crea el listener, el objeto que recibe conexiones nuevas. |
| `QuicAddrSetFamily(&dir, QUIC_ADDRESS_FAMILY_UNSPEC)` / `QuicAddrSetPort(&dir, 7000)` | dirección `QUIC_ADDR`, familia / puerto | Arman la dirección local: todas las interfaces (como `INADDR_ANY`) en el puerto 7000. `QuicAddrSetPort` hace el `htons`. |
| `ListenerStart(listener, &alpn, 1, &dir)` | listener, ALPN y cantidad, dirección | Empieza a escuchar. Por debajo crea el socket UDP en el puerto 7000 (≈ `bind` + `listen`). |
| `SetCallbackHandler(handle, funcion, contexto)` | handle de conexión o stream, callback, contexto | Asigna el callback que atenderá los eventos de una conexión nueva (contexto = su posición en la tabla) o de un stream abierto por el cliente. |
| `ConnectionSetConfiguration(conexion, config)` | conexión entrante, configuración | Le da a la conexión entrante el certificado y los parámetros. Con esto msquic completa el handshake. Se llama en `NEW_CONNECTION`. |
| `StreamShutdown(stream, flags, 0)` | stream, `GRACEFUL` (cerrar ordenadamente) o `ABORT` (abortar), código de error | Cuando el cliente cierra su lado del stream, el broker cierra el suyo. |
| `StreamClose(stream)` | stream | Libera el handle del stream en `SHUTDOWN_COMPLETE` (antes se quita de la tabla, con el mutex tomado). |
| `ConnectionClose(conexion)` | conexión | Libera el handle de la conexión en `SHUTDOWN_COMPLETE` y libera la posición de la tabla. |
| `ListenerClose(listener)` | listener | Deja de aceptar conexiones (al presionar Enter). |
| `RegistrationShutdown(registro, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0)` | registro, flags, código | Cierra todas las conexiones abiertas. Los clientes ven `SHUTDOWN_INITIATED_BY_PEER`. |

### Publicador y suscriptor

| Función | Parámetros | Qué hace en el programa |
|---|---|---|
| `ConnectionOpen(registro, callback_conexion, NULL, &conexion)` | registro, callback, contexto, salida | Crea la conexión, todavía sin conectar (≈ `socket()`). |
| `ConnectionStart(conexion, config, QUIC_ADDRESS_FAMILY_UNSPEC, ip, 7000)` | conexión, configuración, familia, IP o nombre del broker en texto, puerto | Inicia el handshake QUIC + TLS 1.3 (≈ `connect()`). Es **asíncrona**: el hilo principal espera el evento `CONNECTED` con una variable de condición. |
| `StreamOpen(conexion, QUIC_STREAM_OPEN_FLAG_NONE, callback_stream, NULL, &stream)` | conexión, flags (NONE = bidireccional), callback, contexto, salida | Crea el stream por donde viajan los mensajes. |
| `StreamStart(stream, QUIC_STREAM_START_FLAG_NONE)` | stream, flags | Activa el stream y le asigna su ID dentro de la conexión. |
| `StreamSend(stream, &buffer, 1, QUIC_SEND_FLAG_NONE, contexto)` | stream, arreglo de `QUIC_BUFFER` {Length, Buffer} y su cantidad, flags, contexto | Encola el mensaje (≈ `send()`). El buffer no se puede liberar hasta el evento `SEND_COMPLETE`, que devuelve el contexto (el mismo puntero) para hacer `free`. *(El broker también la usa para reenviar.)* |
| `StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0)` | stream, flags, código | El publicador envía FIN al terminar y espera hasta 5 s a que se entregue todo, para no perder los últimos eventos. |
| `ConnectionShutdown(conexion, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0)` | conexión, flags, código | Cierra la conexión (envía un frame `CONNECTION_CLOSE`). |
| `ConnectionClose`, `ConfigurationClose`, `RegistrationClose`, `MsQuicClose` | el handle / la tabla | Liberan todo en orden inverso al de creación. |

### Eventos (callbacks) atendidos

| Callback | Evento | Acción |
|---|---|---|
| listener | `QUIC_LISTENER_EVENT_NEW_CONNECTION` | Busca una posición libre en la tabla (si no hay, devuelve `QUIC_STATUS_CONNECTION_REFUSED`) y luego llama `SetCallbackHandler` + `ConnectionSetConfiguration`. |
| conexión | `CONNECTED` | Handshake terminado. El cliente despierta al hilo principal. |
| conexión | `PEER_STREAM_STARTED` | (broker) El cliente abrió su stream: se guarda en la tabla y se le asigna `callback_stream`. |
| conexión | `SHUTDOWN_INITIATED_BY_TRANSPORT` | Cierre por el transporte: timeout de inactividad (`Timer expired`, `Connection timed out`) o puerto inalcanzable (`No route to host`, que es el ICMP *port unreachable* cuando el broker no está corriendo). |
| conexión | `SHUTDOWN_INITIATED_BY_PEER` | El otro extremo cerró la conexión. |
| conexión | `SHUTDOWN_COMPLETE` | Ya se puede cerrar el handle (`ConnectionClose`). |
| stream | `RECEIVE` | Llegaron bytes en `Buffers[0..BufferCount-1]`: se acumulan y se procesa cada línea completa. |
| stream | `SEND_COMPLETE` | Se libera el buffer de un `StreamSend`. |
| stream | `PEER_SEND_SHUTDOWN` / `PEER_SEND_ABORTED` | El otro extremo cerró o abortó su lado del stream: se cierra el propio con `StreamShutdown`. |
| stream | `SHUTDOWN_COMPLETE` | `StreamClose`. |

## 7. Captura con Wireshark

```bash
sudo tcpdump -i any -w quic_pubsub.pcap udp port 7000     # o Wireshark con el filtro: udp.port == 7000
```

Wireshark decodifica el tráfico como **QUIC**:
- **Initial** (cabecera larga): Wireshark sí puede descifrarlos, porque sus claves se derivan del Connection ID. Ahí se ven el `ClientHello`/`ServerHello` de TLS 1.3 y el ALPN `pubsub`. En un solo RTT se negocian transporte y cifrado, frente al SYN/SYN-ACK/ACK de TCP más un handshake TLS aparte.
- **Handshake** y paquetes **1-RTT** (cabecera corta, "Protected Payload"): van **cifrados**. Ahí viajan los frames STREAM con las noticias, los ACK, los PING de keep-alive (un paquete pequeño cada 5 s mientras no hay tráfico) y el `CONNECTION_CLOSE`. Wireshark no puede mostrar ni el texto ni el tipo de frame, porque QUIC cifra incluso sus propios mensajes de control. Esa es una diferencia clave con TCP, donde los SYN/ACK/FIN y el payload se ven en claro.
- Se identifican los Connection IDs y el puerto origen/destino UDP. El número de paquete también va protegido.

En las capturas locales (`-i lo`), cada conexión empieza con datagramas de ~1200 bytes. Ese relleno (*padding*) es obligatorio en el paquete Initial de QUIC: evita que el protocolo se use para amplificar ataques.
