# Cómo enviar el driver al kernel (dri-devel)

Guía paso a paso para mandar este driver a la lista de correo del kernel.
Resume `Documentation/process/submitting-patches.rst`,
`Documentation/process/coding-style.rst`,
`Documentation/process/submit-checklist.rst` y
`Documentation/gpu/introduction.rst` aplicados a este driver. Leer esos
documentos antes del primer envío vale la pena; esto es el atajo.

## 0. Antes de nada: qué falta probar en hardware

El código compila sin warnings (`W=1`) y pasa `checkpatch --strict`, pero los
cambios de esta preparación **no se han probado en el adaptador**. Antes de
enviar hay que confirmar en el hardware real:

1. El arreglo de IRQ-starvation (commit `082a6a4`, pendiente desde antes):
   sin lag general y `dmesg | grep -i fence` limpio.
2. Que todo sigue igual tras la limpieza: arranque con el adaptador
   conectado, EDID, escritorio GNOME a 1920x1080, desconectar el adaptador
   sin cuelgue. `sudo bash scripts/fl2000-field-test.sh` desde una TTY.
3. Los mensajes de éxito ahora son de depuración (el kernel pide drivers
   silenciosos). Para verlos en el field test:
   `echo 'options fl2000 dyndbg=+p' | sudo tee /etc/modprobe.d/fl2000-debug.conf`
   (y para los de DRM, `drm.debug=0x04` en la línea del kernel).
4. El nombre del driver DRM cambió de `fl2000_drm` a `fl2000`
   (`modetest -M fl2000`; el script ya está actualizado).

Opcional, pero recomendable antes de la v1: probar
`docs/pending/0001-fix-actually-try-the-htotal-adjustments-when-searchi.patch`
(`git am` sobre esta rama). Arregla un bug real (el modo 1920x1080 a 60.00 Hz
se rechaza y GNOME usa 59.94 Hz), pero cambia el modo que recibe tu monitor,
así que solo debe entrar si se ve bien en pantalla.

## 1. Árbol base: drm-misc-next

Los drivers DRM pequeños entran por **drm-misc**, no por el árbol de Linus.
Los parches deben aplicar sobre `drm-misc-next`:

```bash
git clone https://gitlab.freedesktop.org/drm/misc/kernel.git -b drm-misc-next linux-drm-misc
cd linux-drm-misc
git config user.name  "Angel Gabriel Crispin Valdivia"
git config user.email "angelcrispinvaldivia@gmail.com"
```

## 2. Generar la serie

```bash
cd ~/fl2000_drm_kernel7.1
scripts/make-upstream-series.sh ~/linux-drm-misc        # deja los .patch en ./outgoing
```

El script copia `drivers/gpu/drm/fl2000/` al árbol, añade las líneas en
`drivers/gpu/drm/Kconfig`, `drivers/gpu/drm/Makefile` y la entrada de
`MAINTAINERS`, hace un commit con el mensaje de `docs/upstream/commit-message.txt`
y genera la serie con la carta de presentación
(`docs/upstream/cover-letter.txt`). Al final ejecuta `checkpatch.pl --strict`
y `get_maintainer.pl`.

**Sobre el `Signed-off-by`:** el commit lleva `Signed-off-by:` con tu nombre.
Con esa línea certificas el *Developer's Certificate of Origin* (sección 11
de submitting-patches): que tienes derecho a enviar el código bajo GPL-2.0.
Aquí aplica el apartado (b): el código viene del driver de Artem Mygaiev
(GPL-2.0), que a su vez incluye código de Fresco Logic, y los avisos de
copyright originales se mantienen en cada archivo. El mensaje del commit lo
reconoce explícitamente. Es buena práctica escribirle a Artem (autor de
github.com/klogg/fl2000_drm) antes de enviar y ponerlo en Cc.

El nombre en el `From:`/`Signed-off-by:` debe ser tu nombre real; no se
aceptan seudónimos ni identidades anónimas.

## 3. Comprobaciones que el script no hace

En el árbol de drm-misc-next, con una configuración que tenga
`CONFIG_DRM_FL2000=m`:

```bash
make olddefconfig
make -j$(nproc) W=1 C=1 drivers/gpu/drm/fl2000/   # C=1 ejecuta sparse
make -j$(nproc) M=drivers/gpu/drm/fl2000 modules
```

- `sparse` no se pudo ejecutar en la preparación: instalarlo
  (`pacman -S sparse`) y confirmar que `C=1` no da avisos.
- Compilar también para 32 bits si es posible (`make ARCH=i386 ...`); los
  revisores y el bot de 0-day lo harán de todas formas.
- Leer los parches enteros (`outgoing/*.patch`) antes de enviarlos.

## 4. Enviar

Reglas de la lista: **solo texto plano**, nada de HTML ni adjuntos, una
respuesta por línea citada (*inline*, debajo de lo citado, nunca
*top-posting*), y líneas de 72-80 columnas en los correos.

### Opción recomendada: b4

`b4` gestiona versiones, carta de presentación y destinatarios, y puede
enviar por el *web endpoint* de kernel.org sin configurar SMTP:

```bash
pip install --user b4
cd ~/linux-drm-misc && git checkout fl2000-upstream
b4 prep --enroll drm-misc-next         # convierte la rama en una serie b4
b4 prep --edit-cover                   # pegar docs/upstream/cover-letter.txt
b4 prep --auto-to-cc                   # rellena To/Cc con get_maintainer.pl
b4 send --web-auth-new                 # solo la primera vez (luego --web-auth-verify)
b4 send --reflect                      # se lo envía solo a ti, para revisar
b4 send                                # envío real
```

### Alternativa: git send-email con Gmail

Gmail requiere una *contraseña de aplicación* (Cuenta de Google → Seguridad →
Verificación en 2 pasos → Contraseñas de aplicaciones).

```bash
git config --global sendemail.smtpServer smtp.gmail.com
git config --global sendemail.smtpServerPort 587
git config --global sendemail.smtpEncryption tls
git config --global sendemail.smtpUser angelcrispinvaldivia@gmail.com

# 1) prueba: enviártelo a ti mismo y comprobar que llega bien formateado
git send-email --to=angelcrispinvaldivia@gmail.com outgoing/*.patch

# 2) envío real: To/Cc según la salida de get_maintainer.pl
git send-email \
  --to=dri-devel@lists.freedesktop.org \
  --cc=linux-kernel@vger.kernel.org \
  --cc="<cada mantenedor que liste get_maintainer.pl>" \
  outgoing/*.patch
```

Para un driver DRM nuevo, `get_maintainer.pl` lista a los mantenedores de
drm-misc (Maarten Lankhorst, Maxime Ripard, Thomas Zimmermann), a David
Airlie y Simona Vetter, y las listas `dri-devel` y `linux-kernel`. Usar
exactamente lo que imprima sobre el árbol actualizado.

## 5. Después del envío

- Comprobar que aparece en https://lore.kernel.org/dri-devel/ y en
  https://patchwork.freedesktop.org/.
- Contestar a cada revisión (aunque sea "de acuerdo, lo cambio en v2").
- No reenviar antes de ~1 semana salvo que lo pidan. Si nadie responde en 2-3
  semanas, un "ping" educado respondiendo al hilo está bien.
- Para la siguiente versión: `VERSION=2 PREFIX="PATCH" scripts/make-upstream-series.sh ...`
  (o `b4 prep` / `b4 send`, que lleva la cuenta solo), y añadir debajo de la
  línea `---` del parche un registro de cambios:

  ```
  ---
  Changes in v2:
  - Convert to regular atomic helpers (Thomas)
  - ...
  ```

- Las etiquetas que den los revisores (`Reviewed-by:`, `Acked-by:`,
  `Tested-by:`) se copian en el mensaje del commit en la siguiente versión.

## 6. Lo que los revisores van a pedir (y por qué se envía como RFC)

La serie se marca `RFC PATCH` porque hay cambios de diseño que conviene
acordar con los mantenedores antes de hacerlos. La carta de presentación ya
los plantea:

1. **IT66121 duplicado.** Ya existe `drivers/gpu/drm/bridge/ite-it66121.c`.
   Lo normal es que pidan reutilizarlo. El obstáculo es real: ese driver se
   instancia por device tree y lee el EDID por el motor DDC del IT66121, que
   en este adaptador nunca funciona (el DDC del monitor cuelga del bus I²C del
   FL2000). La carta propone instanciarlo desde el driver USB con un
   *software node* y usar `drm_bridge_connector` con el DDC del FL2000.
2. **`drm_simple_display_pipe` está obsoleto** para drivers nuevos: habrá que
   pasar a plano/CRTC/encoder con los helpers atómicos normales, y que el
   bridge soporte `DRM_BRIDGE_ATTACH_NO_CONNECTOR`.
3. **Vblank falso:** se señala en cada URB completado. Lo esperable es usar el
   temporizador de vblank de DRM (`drm_crtc_vblank_start_timer()` /
   `DRM_CRTC_VBLANK_TIMER_FUNCS`) y pacing a la tasa de refresco. Coincide con
   el "Open issue 2" de `CLAUDE.md`.
4. **Sin fbdev** (`drm_client_setup()`), sin suspend/resume real, y las
   *damage clips* no se usan.
5. El IT66121 usa los callbacks legacy (`enable`/`disable`/`mode_set`) en vez
   de `atomic_*`.
6. El estado compartido (regmap, adaptador I²C, IT66121) se guarda como
   `drvdata`/devres del `struct usb_device`, que pertenece al driver genérico
   `usb`, no a una interfaz nuestra. Es lo que permite sobrevivir a
   `usb_reset_device()`, pero es poco habitual y probablemente lo
   cuestionen; la carta de presentación lo pregunta (punto 4).

Ninguno de estos puntos se ha tocado todavía porque cambian el
comportamiento de un driver que funciona y no se pueden validar sin el
adaptador; cada uno es una tarea para hacer con el hardware delante.
