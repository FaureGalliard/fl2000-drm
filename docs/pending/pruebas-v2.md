# Pruebas en el adaptador de los arreglos para la v2

Rama `fix/sashiko-v1`. En la VM todo pasa (`docs/vm-test-v2.log`), pero la VM no
sabe nada del chip real. Esto es lo que solo se ve con el adaptador. Cada punto dice
qué cambió, qué debería verse y qué mirar si no.

## Instalar

```bash
cd ~/Projects/fl2000_drm_kernel7.1
git checkout fix/sashiko-v1
make clean && make
sudo cp /lib/modules/$(uname -r)/extra/fl2000.ko ~/fl2000-integration.ko   # para volver atrás
sudo install -D -m 644 fl2000.ko /lib/modules/$(uname -r)/extra/fl2000.ko && sudo depmod -a
```

Reiniciar con el adaptador conectado. Para volver atrás: copiar
`~/fl2000-integration.ko` al mismo sitio, `sudo depmod -a` y reiniciar.

## 1. Arranca (lo primero y lo más arriesgado)

```bash
scripts/post-boot-check.sh
journalctl -k -b | grep -E 'IT66121|fl2000|EDID' | head -30
```

- `IT66121 found, revision N` (con `dyndbg=+p`): por primera vez se lee el chip de
  verdad durante el probe. **Si sale `No IT66121 found`, nada arranca:** apuntar las
  líneas `No answer at address` o `Not an IT66121 at address` y volver atrás. El
  remedio sería hacer esa lectura más tarde en el probe.
- Ya no debe salir `DDC not done` ni `EDID FIFO read failed`, y el adaptador tarda
  unos 650 ms menos en aparecer.
- Tampoco `Adapter does not support I2C functions properly`.

## 2. HDMI

- GNOME en el monitor externo a 1920x1080, como siempre. **Si solo ofrece 1024x768 y
  sale `EDID read failed`:** falla la lectura directa sin el intento previo por FIFO
  (commit `6477ff2`).
- El cursor, sobre todo en sus bordes sobre fondo claro: antes salían algo oscuros,
  ahora deberían verse bien. Si se ven demasiado claros, Mutter no los manda
  premultiplicados, lo contrario de lo que asumimos (commit `37d6e6d`).
- Arrastrar una ventana: `Stream:` en el log a 59 sent/s, sin `fence fallback`.

## 3. Conectar y desconectar el monitor (el cambio de comportamiento)

En otra terminal: `udevadm monitor -k -s drm`.

- Desconectar el cable HDMI con GNOME abierto. Antes tardaba hasta 10 s: ahora, si la
  línea de interrupción del IT66121 llega al FL2000 (no lo sabemos, nunca se había
  activado), debería notarse al instante. Si sigue tardando 10 s no pasa nada grave:
  anotarlo, porque responde a la pregunta 2 de la carta.
- **Lo importante: unos pocos eventos por cada conexión, no una lluvia continua.** Si
  no paran, es el reconocimiento de la interrupción (commits `2c2eed1`, `0a70cca`,
  `9764d18`).
- **Sin monitor VGA conectado, al quitar HDMI no debe aparecer un monitor VGA fantasma**
  en Configuración de GNOME (commit `97e7934`).
- Volver a conectar: el monitor vuelve solo.

## 4. VGA (el LG 19EN43, en casa)

- Con el cable VGA y nada en HDMI, el conector sale `connected` y GNOME va a 1366x768.
- Desconectar y volver a conectar el cable VGA.
- Pasar de HDMI a VGA y de vuelta sin cerrar sesión: tras quitar HDMI, VGA aparece
  cuando GNOME apaga el CRTC.

## 5. Suspender

`systemctl suspend` con GNOME abierto, despertar con el botón de encendido: la imagen
vuelve y no hay `resume error` ni `Cannot restore`. La misma lógica sirve ahora para
un reset USB.

## 6. Reset USB (nuevo)

Con GNOME abierto, y teniendo a mano `usbreset` (paquete `usbutils`):

```bash
sudo usbreset 1d5c:2000
```

La imagen parpadea y vuelve **sin que GNOME vea una desconexión**; el `cardN` sigue
siendo el mismo. Antes era una desconexión completa. Es lo que pasa cuando el
CD-ROM virtual da un timeout (sin la quirk de `usb-storage`).

## 7. Desenchufar el adaptador

Con GNOME abierto, desenchufar. No debe colgarse nada; esperar 10 s y volver a
enchufar. `lsusb -d 1d5c:2000` debe verlo. Lo que haga Mutter después es lo que ya
sabemos (ver "Unplug and replug" en `docs/DEVELOPMENT.md`).

**Si se cuelga** el rework es lo primero que hay que mirar (commit `eeb2f11`).
`journalctl -b -1 -k | tail` dice dónde se quedó.

## 8. rmmod (solo al final, antes crasheaba el kernel)

Desde una TTY y cerrando la sesión gráfica:

```bash
sudo systemctl stop gdm
sudo rmmod fl2000            # con el adaptador conectado
# desenchufar y volver a enchufar el adaptador
sudo modprobe fl2000
sudo systemctl start gdm
```

## Para terminar

`sudo bash scripts/fl2000-field-test.sh` desde una TTY y añadir el log a `docs/`.
