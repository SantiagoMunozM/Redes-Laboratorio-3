# Laboratorio 3 - Publicación/Suscripción de noticias deportivas (TCP y UDP)

Tres roles, en C sobre Linux:

- **broker**: recibe todas las noticias y las reenvía, sin modificarlas, solo a los subscribers de ese partido.
- **publisher**: el periodista; publica eventos de un partido.
- **subscriber**: el hincha; sigue uno o varios partidos.

El broker escucha en el puerto **8080** en todas sus interfaces (en TCP y en UDP; son espacios de puertos distintos y no chocan).

El publisher y el subscriber reciben la **IP del broker como primer argumento**: `127.0.0.1` si todo corre en la misma máquina, o la IP de la máquina del broker si cada rol corre en una máquina distinta.

## Compilar

Desde la raíz del repo:

```bash
cd tcp
gcc -Wall broker_tcp.c -o broker_tcp
gcc -Wall publisher_tcp.c -o publisher_tcp
gcc -Wall subscriber_tcp.c -o subscriber_tcp

cd ../udp
gcc -Wall broker_udp.c -o broker_udp
gcc -Wall publisher_udp.c -o publisher_udp
gcc -Wall subscriber_udp.c -o subscriber_udp
```

QUIC tiene sus propias instrucciones en [quic/README.md](quic/README.md).

## Uso

| Programa | Comando |
|---|---|
| broker | `./broker_tcp` / `./broker_udp` |
| subscriber | `./subscriber_tcp <ip_broker> <partido1> [partido2 ...]` (máx. 10 partidos) |
| publisher | `./publisher_tcp <ip_broker> <partido> [auto\|manual]` |

El UDP se usa igual, cambiando `_tcp` por `_udp`.

- `<ip_broker>` es una dirección IPv4 (por ejemplo `127.0.0.1` o `192.168.36.130`); no se aceptan nombres de host.
- El nombre del partido no puede estar vacío, ni contener `|`, ni pasar de 49 caracteres.
- **auto** (por defecto): el publisher envía 10 eventos de ejemplo, uno por segundo.
- **manual**: cada línea que escribas es un evento. Termina con `exit` o Ctrl+D.

## Prueba mínima del enunciado

1 broker, 2 subscribers, 2 publishers, mínimo 10 mensajes por publisher. Usa **una terminal por programa**, en este orden (el broker no guarda noticias para quien llegue tarde). El ejemplo corre todo en una sola máquina, por eso la IP es `127.0.0.1`:

```bash
# Terminal 1: broker
./broker_tcp

# Terminal 2: subscriber de los dos partidos
./subscriber_tcp 127.0.0.1 ColombiaVsBrasil ArgentinaVsChile

# Terminal 3: subscriber de un solo partido
./subscriber_tcp 127.0.0.1 ArgentinaVsChile

# Terminal 4 y 5: publishers (modo auto: 10 eventos cada uno)
./publisher_tcp 127.0.0.1 ColombiaVsBrasil
./publisher_tcp 127.0.0.1 ArgentinaVsChile
```

Resultado esperado:

- La terminal 2 recibe las 20 noticias (10 de cada partido).
- La terminal 3 recibe solo las 10 de `ArgentinaVsChile`.
- El broker imprime a cuántos suscriptores reenvió cada noticia.

Para UDP es igual, con `./broker_udp`, `./subscriber_udp` y `./publisher_udp`.

Para probar el modo manual: `./publisher_tcp 127.0.0.1 ColombiaVsBrasil manual`, escribe un evento por línea y cierra con `exit`.

## Correr cada rol en una máquina distinta

1. En la máquina del broker, mira su IP con `ip -4 a` (la de la interfaz de red, no `127.0.0.1`) y arranca `./broker_tcp`.
2. Si el firewall está activo en esa máquina, abre el puerto: `sudo ufw allow 8080/tcp` y `sudo ufw allow 8080/udp`.
3. En las otras máquinas usa esa IP como primer argumento:

```bash
./subscriber_tcp 192.168.36.130 ColombiaVsBrasil ArgentinaVsChile
./publisher_tcp  192.168.36.130 ColombiaVsBrasil
```

Solo el broker necesita ser alcanzable; el publisher y el subscriber únicamente inician la comunicación. Para comprobar que se llega al broker: `nc -zv <ip_broker> 8080` (TCP) o `nc -zvu <ip_broker> 8080` (UDP).

## Diferencias entre TCP y UDP

- **TCP**: cada mensaje termina en `\n` porque TCP es un flujo de bytes y no conserva los límites de los mensajes. El broker usa `select()` para atender a todos los clientes en un solo hilo. Si el broker se cierra, los subscribers lo detectan y terminan.
- **UDP**: cada datagrama es un mensaje (sin `\n`). El broker identifica a cada subscriber por su IP y puerto de origen. Si el broker se cae, el subscriber no se entera y hay que cerrarlo con Ctrl+C. Si el registro de un subscriber se pierde, no recibirá noticias, y no hay forma de darse de baja.

## Capturas de tráfico (Wireshark / tcpdump)

La interfaz es `lo` (loopback) cuando todo corre en la misma máquina. Con máquinas distintas, captura en la del broker con `-i any` en lugar de `-i lo`. Inicia la captura **antes** de arrancar los programas y deténla con Ctrl+C al terminar la prueba:

```bash
sudo tcpdump -i lo tcp port 8080 -w tcp_pubsub.pcap
sudo tcpdump -i lo udp port 8080 -w udp_pubsub.pcap
```

Los `.pcap` se abren con Wireshark. Filtros útiles: `tcp.port == 8080` y `udp.port == 8080`.

## Problemas comunes

- **`Error al asociar el socket`** en el broker: el puerto 8080 está ocupado por otro programa (¿un broker que quedó abierto?). Ciérralo o espera unos segundos.
- **`IP del broker invalida`**: falta la IP como primer argumento, o no es una dirección IPv4.
- **`Error al conectar con el servidor`** (TCP): el broker no está corriendo, la IP no es la suya o un firewall bloquea el puerto 8080. Inícialo primero.
- **UDP sin errores pero sin noticias**: el broker no estaba corriendo cuando se registró el subscriber. En UDP no hay aviso; cierra el subscriber y arráncalo de nuevo con el broker activo.
- **En macOS** compilan TCP y UDP sin problema; para la captura usa la interfaz `lo0` en lugar de `lo`.
