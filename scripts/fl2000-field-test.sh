#!/bin/bash
# Prueba de campo del driver fl2000: recolecta toda la evidencia en un solo run.
#
# Uso:  sudo ./scripts/fl2000-field-test.sh
#
# Recomendado: correrlo desde una TTY (Ctrl+Alt+F3) para que la prueba de
# modeset no choque con el master DRM de GNOME. Tambien funciona desde la
# sesion grafica (la prueba de modeset puede fallar con Permission denied,
# el resto de la evidencia se captura igual).
#
# El resultado queda en docs/field-test-HH_MM.log — hacer commit y push.

set -u

cd "$(dirname "$0")/.." || exit 1
mkdir -p docs
OUT="docs/field-test-$(date +%H_%M).log"
exec > >(tee "$OUT") 2>&1

step() { echo; echo "=== $* ==="; }

echo "fl2000 field test - $(date)"
echo "kernel: $(uname -r)"

step "quirk usb-storage (debe contener 1d5c:2000:i)"
modprobe usb_storage 2>/dev/null
# El archivo modprobe.d no aplica si usb_storage se cargo desde un initramfs
# viejo (correr 'sudo mkinitcpio -P' una vez lo arregla permanente).
# Aplicar en caliente por si acaso:
echo '1d5c:2000:i' > /sys/module/usb_storage/parameters/quirks 2>/dev/null
cat /sys/module/usb_storage/parameters/quirks 2>/dev/null || echo "usb_storage no cargado"

# Si usb-storage ya esta unido al CD virtual del FL2000 (conectado desde el
# arranque), soltarlo para que no dispare resets SCSI a los 30s
for intf in /sys/bus/usb/drivers/usb-storage/[0-9]*; do
	[ -e "$intf" ] || continue
	vid=$(cat "$intf/../idVendor" 2>/dev/null)
	pid=$(cat "$intf/../idProduct" 2>/dev/null)
	if [ "$vid" = "1d5c" ] && [ "$pid" = "2000" ]; then
		basename "$intf" > /sys/bus/usb/drivers/usb-storage/unbind
		echo "usb-storage desatado de la interfaz $(basename "$intf") del FL2000"
	fi
done

step "modulo fl2000"
if ! lsmod | grep -q '^fl2000'; then
	modprobe fl2000 && echo "fl2000 cargado ahora" || echo "ERROR: no se pudo cargar fl2000"
else
	lsmod | grep '^fl2000'
fi

step "adaptador USB"
if ! lsusb -d 1d5c:2000; then
	echo ">>> CONECTA EL ADAPTADOR AHORA (con el monitor encendido) <<<"
fi

step "esperando tarjeta DRM del fl2000 (max 90s)"
FOUND=0
for i in $(seq 1 45); do
	if modetest -M fl2000 -c >/dev/null 2>&1; then
		FOUND=1
		echo "tarjeta DRM presente (tras $((i * 2))s)"
		break
	fi
	if [ "$i" = 15 ] && ! lsusb -d 1d5c:2000 >/dev/null; then
		echo ">>> El adaptador NO esta en el bus USB. Si lo reconectaste hace"
		echo ">>> poco, la parte USB 3.0 puede no haber enumerado: dejalo"
		echo ">>> DESCONECTADO 10 segundos y enchufalo firme otra vez <<<"
	fi
	sleep 2
done
if [ "$FOUND" = 0 ]; then
	echo "ERROR: la tarjeta DRM del fl2000 nunca aparecio."
	if lsusb -d 1d5c:2000 >/dev/null; then
		echo "El adaptador SI esta en USB: el probe fallo, revisar dmesg abajo."
	else
		echo "El adaptador no esta en el bus USB (fallo de enumeracion USB3):"
		echo "dejalo desconectado 10s, reconectalo firme y repite el script."
		echo "Lo mas fiable: arrancar Linux con el adaptador ya conectado."
	fi
fi

step "estado /sys/class/drm"
ls /sys/class/drm/
for f in /sys/class/drm/card*-*/status; do echo "$f: $(cat "$f")"; done

step "conectores y modos (modetest -c)"
modetest -M fl2000 -c 2>&1 | head -40

step "EDID decodificado (si hay edid-decode)"
for e in /sys/class/drm/card*-HDMI-A-2/edid; do
	if [ -s "$e" ]; then
		if command -v edid-decode >/dev/null; then
			edid-decode <"$e" | head -30
		else
			echo "$e tiene $(stat -c%s "$e") bytes (instala edid-decode para ver detalle)"
		fi
	else
		echo "$e vacio o inexistente"
	fi
done

if [ "$FOUND" = 1 ]; then
	CONN=$(modetest -M fl2000 -c 2>/dev/null | awk '/\<connected\>/{print $1; exit}')
	# Primer modo de la lista (el preferido del monitor), sin fijar refresh:
	# p.ej. el LG lista 1920x1080@59.94 y "1920x1080-60" no haria match
	MODE=$(modetest -M fl2000 -c 2>/dev/null | awk '/#0 /{print $2; exit}')
	if [ -n "${CONN:-}" ] && [ -n "${MODE:-}" ]; then
		step "PRUEBA DE MODESET: patron de colores 15s en conector $CONN modo $MODE"
		echo ">>> MIRA EL MONITOR LG AHORA - deberian verse barras de colores <<<"
		sleep 12 | modetest -M fl2000 -s "$CONN:$MODE" 2>&1
		echo "(anota si se vio el patron y compartelo junto con este log)"
	else
		step "PRUEBA DE MODESET omitida: sin conector conectado o sin modos"
	fi
fi

step "dmesg (ultimas 120 lineas)"
dmesg | tail -120

echo
echo "Listo. Log guardado en $OUT - haz commit y push para compartirlo."
