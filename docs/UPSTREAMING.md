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

## 0b. Antes de enviar: hablar con Artem Mygaiev (klogg)

Investigado el 26 de octubre de 2026:

- **Nadie ha enviado nunca un driver FL2000 a las listas del kernel**, y en
  mainline no hay soporte para FL2000. La búsqueda se hizo por índices web
  porque lore.kernel.org no era accesible desde el entorno de preparación;
  compruébalo tú: <https://lore.kernel.org/all/?q=fl2000> y `?q=1d5c`.
- **Pero el autor original está activo otra vez.** Entre el 17 y el 19 de
  octubre de 2026, Artem subió decenas de commits a
  <https://github.com/klogg/fl2000_drm>: quitó `simple_display_pipe`, adaptó el
  driver IT66121 del kernel, apunta a Linux 7.0 y ya hace la selección
  RGB888/RGB565 por ancho de banda. En el PR #86
  (<https://github.com/klogg/fl2000_drm/pull/86>) escribió que publicará en el
  kernel "por separado" el trabajo del IT66121.
- Samuv5 borró el 22 de octubre su modernización de mayo y junio (de la que
  parte este fork) diciendo que "compilaba pero no funcionaba", y la ha
  reescrito; según su README, la parte HDMI está sin probar en hardware.
- Hay otros forks activos: rhgndf (VGA, autor del PR #86) y
  vfox888-cpu/alpha-fl2000 (escrito desde cero).

Mandar un RFC sin avisar chocaría con el autor del código del que deriva este
driver, y eso perjudica la revisión más que cualquier detalle de estilo.
Escríbele antes (issue o correo, `joculator@gmail.com` según el historial).
Lo que este fork aporta y su rama no tiene probado en tu hardware:

- el EDID por el bus I²C del FL2000;
- el ciclo de vida del bridge a través de `usb_reset_device()`;
- el arreglo del lag por IRQ;
- las correcciones de esta preparación.

Hay tres caminos razonables: que él lo envíe con tus parches (te da
`Co-developed-by:` + `Signed-off-by:` o `Tested-by:`, y también cuenta como
contribución al kernel), enviarlo juntos, o que te ceda el envío.

## Uso de IA: `Assisted-by`

`Documentation/process/coding-assistants.rst` (en el árbol del kernel) dice:

- Una IA nunca pone `Signed-off-by`. Solo lo pone el humano, que certifica el
  DCO y se responsabiliza del código.
- Las contribuciones en las que ha participado una herramienta de IA deberían
  llevar `Assisted-by: AGENTE:MODELO [herramientas]`.

Buena parte de esta preparación se hizo con ayuda de IA, así que la regla
aplica. El script no añade la etiqueta por defecto. Para incluirla:
`ASSISTED_BY="AGENTE:MODELO" scripts/make-upstream-series.sh ...`, con el
nombre y la versión exactos de la herramienta que usaste. No declararlo y que
luego se note suele sentar peor a los revisores que declararlo. La decisión es
tuya.

## Calendario realista

- **Ritmo:** sale un kernel cada 9 semanas (10 si cae en vacaciones). Últimas
  versiones: 7.1 el 14 de junio de 2026, 7.2 el 16 de agosto y 7.3 el 18 de
  octubre. Ahora está abierta la ventana de merge de 7.4.
- **Plazo para drivers nuevos:** tienen que estar aplicados en drm-misc-next
  antes del -rc6 del ciclo anterior (`Documentation/gpu/introduction.rst`,
  "Feature Merge Deadlines").
- **Proyección:**
  - **7.5** sale hacia finales de febrero de 2027. Su plazo es el 7.4-rc6,
    hacia el 6 de diciembre de 2026, y no da tiempo.
  - **7.6** sale hacia finales de abril de 2027. Es realista si el RFC sale
    pronto.
  - **7.7** sale hacia finales de junio de 2027. Es lo esperable si piden
    reutilizar el IT66121 del kernel.
- **Ningún kernel que salga en enero** puede llevar este driver: el que se
  publique hacia final de diciembre (7.4) ya cerró su plazo.
- **Rondas de revisión:** los drivers DRM nuevos recientes necesitaron entre 4
  y 11 versiones a lo largo de 2 a 5 meses (appletbdrm v7, Sharp Memory LCD
  v11).
- **Badge de GitHub:** el commit se atribuye por el email del autor
  (`From:`). `angelcrispinvaldivia@gmail.com` tiene que estar añadido y
  verificado en tu cuenta de GitHub. Aparece cuando Linus lo fusiona en su
  árbol, es decir, cuando sale el rc1 de esa versión.

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
`MAINTAINERS`, crea **4 commits** (uno por parche, mensajes en
`docs/upstream/series/1..4.txt`; las versiones intermedias de los archivos
que crecen están en `docs/upstream/stages/`)
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
- La preparación ya compiló el driver dentro del árbol v7.1 para x86_64 e
  i386 con `W=1` sin avisos. Repetirlo sobre drm-misc-next (y si se puede,
  `ARCH=arm64` con un cross-compiler): el bot 0-day compila muchas
  arquitecturas y responde a la lista si algo falla.
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
los plantea.

**Ya hecho (migración de octubre de 2026, pendiente de probar en hardware):**

- planos/CRTC/encoder propios en lugar de `drm_simple_display_pipe`, con el
  cálculo del PLL en `atomic_check`;
- *shadow-plane helpers* y `drm_fb_xrgb8888_to_rgb888()`/`_rgb565()`, con el
  intercambio de palabras sobre el búfer completo (esto arregla los anchos
  raros, como 1366 px);
- vblank por el temporizador de DRM (`DRM_CRTC_VBLANK_TIMER_FUNCS`), a la tasa
  del modo;
- fbdev con `drm_client_setup()`;
- callbacks `atomic_enable`/`atomic_disable` en el bridge, ya que los antiguos
  desaparecen en 7.4;
- `REGCACHE_MAPLE` y `kzalloc_objs()`.

**Pendiente:**

1. **IT66121 duplicado.** Ya existe `drivers/gpu/drm/bridge/ite-it66121.c`, y
   lo normal es que pidan reutilizarlo. El obstáculo es real: ese driver se
   instancia por device tree y lee el EDID por el motor DDC del IT66121, que
   en este adaptador nunca funciona porque el DDC del monitor cuelga del bus
   I²C del FL2000. La carta propone instanciarlo desde el driver USB con un
   *software node* y usar `drm_bridge_connector` con el DDC del FL2000.
2. **Bridge sin conector propio** (`DRM_BRIDGE_ATTACH_NO_CONNECTOR` +
   `drm_bridge_connector`). Para compilar fuera del árbol hace falta
   `CONFIG_DRM_BRIDGE_CONNECTOR`, que en x86 solo está activo si algún driver
   lo selecciona. Compruébalo en tu laptop con
   `zgrep DRM_BRIDGE_CONNECTOR /proc/config.gz` antes de hacerlo.
3. **Callbacks antiguos que quedan en el bridge** (`mode_set`) e infoframes a
   mano: lo moderno es `DRM_BRIDGE_OP_HDMI`.
4. **Estado compartido en el `struct usb_device`.** El regmap, el adaptador
   I²C y el IT66121 se guardan como `drvdata`/devres del dispositivo USB, que
   pertenece al driver genérico `usb` y no a una interfaz nuestra. Es lo que
   permite sobrevivir a `usb_reset_device()`, pero es poco habitual y
   probablemente lo cuestionen; la carta lo pregunta.
5. **Framework de componentes.** `drivers/gpu/drm/drm_drv.c` desaconseja usar
   componentes cuando existe una interfaz estándar como `drm_bridge`. Lo
   esperable es crear el bridge en el probe y usar `drm_bridge_attach()`.
6. **El dispositivo DRM cuelga del adaptador I²C.** Lo esperable es que cuelgue
   de la interfaz USB, con
   `drm_dev_set_dma_dev(drm, usb_intf_get_dma_device(intf))`, como hacen udl,
   gud y appletbdrm. Afecta a PRIME y a la topología de udev.
7. **Una interfaz y no tres.** Lo idiomático es enlazar una sola interfaz y
   reclamar las otras con `usb_driver_claim_interface()`, con el estado ligado
   a la interfaz o a `drmm_*`. Klogg y el PR #86 ya lo hicieron así.
8. **Suspend/resume real** (`drm_mode_config_helper_suspend()`).

Los puntos 4 a 7 son un solo cambio grande: sustituyen el modelo de ciclo de
vida que arregló los cuelgues al desconectar (ver "Non-obvious invariants" en
`CLAUDE.md`). Hay que hacerlo con el adaptador delante.
