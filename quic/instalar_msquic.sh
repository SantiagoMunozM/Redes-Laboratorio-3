#!/bin/bash
# Instala la libreria msquic en Ubuntu/Debian, descarga sus headers y genera el certificado TLS.
# Uso: ./instalar_msquic.sh   (pide la clave de sudo)
set -e # detenerse ante el primer error

# carpeta donde esta este script (quic/), para dejar ahi include/ y el certificado
CARPETA="$(cd "$(dirname "$0")" && pwd)"

# 1. Herramientas basicas: compilador, openssl (certificado) y curl (descargas)
sudo apt-get update
sudo apt-get install -y gcc libc6-dev openssl curl ca-certificates

# 2. Repositorio de paquetes de Microsoft (ahi esta publicado libmsquic)
#    /etc/os-release dice la distribucion (ubuntu/debian) y su version (22.04, 24.04, 12...)
. /etc/os-release
if ! dpkg -s packages-microsoft-prod >/dev/null 2>&1; then
    curl -sSfL -o /tmp/packages-microsoft-prod.deb \
        "https://packages.microsoft.com/config/${ID}/${VERSION_ID}/packages-microsoft-prod.deb"
    sudo dpkg -i /tmp/packages-microsoft-prod.deb
    sudo apt-get update
fi

# 3. La libreria (solo trae el binario libmsquic.so.2, no los headers .h)
sudo apt-get install -y libmsquic

# 4. Headers de la MISMA version instalada, tomados del repositorio oficial en GitHub
VERSION=$(dpkg-query -W -f='${Version}' libmsquic | cut -d'-' -f1)
echo "Version de libmsquic instalada: ${VERSION}"
mkdir -p "${CARPETA}/include"
for ARCHIVO in msquic.h msquic_posix.h quic_sal_stub.h; do
    curl -sSfL --max-time 60 -o "${CARPETA}/include/${ARCHIVO}" \
        "https://raw.githubusercontent.com/microsoft/msquic/v${VERSION}/src/inc/${ARCHIVO}"
    echo "Descargado include/${ARCHIVO}"
done

# 5. Certificado autofirmado para el broker (QUIC siempre usa TLS 1.3)
#    solo lo usa la maquina del broker, pero generarlo en todas no hace dano
if [ ! -f "${CARPETA}/server.cert" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=broker" \
        -keyout "${CARPETA}/server.key" -out "${CARPETA}/server.cert"
    echo "Certificado generado: server.cert / server.key"
fi

echo ""
echo "Listo. Para compilar (dentro de la carpeta quic/):"
echo "  gcc -Wall broker_quic.c     -o broker_quic     -Iinclude -l:libmsquic.so.2 -lpthread"
echo "  gcc -Wall publisher_quic.c  -o publisher_quic  -Iinclude -l:libmsquic.so.2 -lpthread"
echo "  gcc -Wall subscriber_quic.c -o subscriber_quic -Iinclude -l:libmsquic.so.2 -lpthread"
