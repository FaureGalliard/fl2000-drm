# Cómo enviar el driver al kernel (dri-devel)

Guía paso a paso para mandar este driver a la lista de correo del kernel.
Resume `Documentation/process/submitting-patches.rst`,
`Documentation/process/coding-style.rst`,
`Documentation/process/submit-checklist.rst` y
`Documentation/gpu/introduction.rst` aplicados a este driver. Leer esos
documentos antes del primer envío vale la pena; esto es el atajo.

## Enviado: RFC v1 (6 de noviembre de 2026, 02:34 UTC)

`[PATCH RFC 0/4] drm: Add Fresco Logic FL2000 USB display driver`, enviado
con `b4 send` (relay web de kernel.org) a los mantenedores de drm-misc y DRM,
`dri-devel` y `linux-kernel`. Está en <https://lore.kernel.org/dri-devel/>
(buscar `fl2000`). b4: `change-id 20261105-fl2000-upstream-3fc2e1f00d24`,
etiqueta `sent/20261105-fl2000-upstream-3fc2e1f00d24-v1` en
`~/linux-drm-misc`; la rama `fl2000-upstream` ya está en la revisión 2.

**Para la v2, no volver a ejecutar `make-upstream-series.sh` sobre la rama
`fl2000-upstream`:** la recrea y se perdería el commit de seguimiento de b4
(carta, destinatarios y `change-id`, que enlaza las versiones). Lo seguro es
generar los cuatro parches en otra rama y llevarlos encima de ese commit:

```bash
cd ~/linux-drm-misc
git checkout --detach 41ecf852c      # la base de la v1 (o un drm-misc-next más nuevo)
BRANCH=fl2000-v2 CHECK_BUILD=1 ~/Projects/fl2000_drm_kernel7.1/scripts/make-upstream-series.sh . /tmp/fl2000-v2
git checkout fl2000-upstream
git reset --hard HEAD~4                     # deja solo el commit de seguimiento de b4
git cherry-pick 41ecf852c..fl2000-v2
EDITOR=nano b4 prep --edit-cover            # el texto está en docs/upstream/v2-changes.txt
b4 prep --check                             # checkpatch y demás
b4 send --reflect                           # primero a ti mismo
b4 send
```

Los parches enviados siguen en la etiqueta `sent/...-v1`, así que el `reset` no
pierde nada. b4 pone `v2` solo. No enviar antes de haberla probado en el adaptador
ni antes de que pase una semana desde la v1 (13 de noviembre), salvo que los
mantenedores contesten antes: `submitting-patches.rst` pide esperar y la respuesta
a las preguntas de la carta puede cambiar el diseño.

**Revisión de Sashiko (6 nov).** Un bot de revisión con IA contestó a los cuatro
parches en menos de una hora. Su análisis y lo hecho están en
`docs/DEVELOPMENT.md`, "Review of the RFC". No hace falta responderle en la lista:
basta con arreglarlo en la v2 y decirlo en el registro de cambios.



Rama `integration`, subida a GitHub. **Todo lo que entra en la serie está
probado en el adaptador** (UGREEN HDMI+VGA, kernel 7.2.11, GNOME 51 en
Wayland):

- HDMI a 1920x1080 (LG 22MP55) y VGA a 1366x768 nativo (LG 19EN43, sin EDID),
  y GNOME pasa de uno a otro sin cerrar sesión. Desenchufar y volver a
  enchufar el cable VGA funciona.
- Los arreglos del 5 de noviembre: fin de frame con paquete de longitud cero
  (1366x768, 800x600, etc.), importación con mapeo (búfer de amdgpu partido
  en VRAM), lectura rápida en anchos raros. La memoria GTT se mantuvo estable.
- El conector HDMI ahora es `drm_bridge_connector` y el IT66121 se engancha
  sin conector propio (`DRM_BRIDGE_ATTACH_NO_CONNECTOR`).
- Suspender y reanudar, también con *reset-resume*: probado con
  `pm_test=devices` y con una suspensión S3 real con la sesión abierta.
- Sin probar en hardware: RGB565 (solo se usa por encima de 2560x1080 en USB
  3.0 o en USB 2.0, ver la tabla de hallazgos de `docs/DEVELOPMENT.md`).

El código compila sin avisos con `W=1` y `sparse` (`C=2`) en 7.2.11 y en
drm-misc-next 7.4-rc1, `checkpatch --strict` no da avisos nuevos y las etapas
reducidas de los parches 1-3 compilan por separado. **La serie de `outgoing/`
y la rama `fl2000-upstream` de `~/linux-drm-misc` son anteriores a todo
esto: hay que regenerarla** (secciones 1 y 2). La genera quien la envía, por
el `Signed-off-by`.

## 0b. klogg

Decidido el 5 de noviembre de 2026: **la serie la envías tú y piensas
mantener el driver**; no hace falta coordinarse con Artem Mygaiev (klogg). El
código es GPL-2.0 y se mantienen sus líneas de copyright y las de Fresco
Logic; la carta dice de dónde viene. Hechos que conviene tener presentes:

- Hasta esa fecha nadie había enviado un driver FL2000 a las listas del
  kernel (compruébalo antes de enviar: <https://lore.kernel.org/all/?q=fl2000>
  y `?q=1d5c`).
- klogg anunció en <https://github.com/klogg/fl2000_drm/pull/86> que enviaría
  el trabajo del IT66121, pero no ha enviado nada. Su IT66121 es una reescritura
  local del driver del kernel, que él mismo describe como no intercambiable con
  el del kernel.
- Reusar el `ite-it66121` del kernel no es posible en un PC sin cambiar otros
  subsistemas (`docs/pending/it66121-in-tree.md`); la pregunta 1 de la carta
  se lo plantea a los mantenedores.
- Si klogg responde al RFC, lo normal es contestarle con cordialidad y
  ofrecer `Co-developed-by:` o `Tested-by:` si aporta.

## Uso de IA

En el árbol 7.4-rc1 (comprobado el 3 de noviembre de 2026) la regla está en dos
documentos, y ninguno de los dos fija ya el formato de una etiqueta
`Assisted-by:`:

- `Documentation/process/coding-assistants.rst`: una IA nunca pone
  `Signed-off-by`. Solo lo pone el humano, que certifica el DCO, revisa todo el
  código y se responsabiliza de él.
- `Documentation/process/generated-content.rst`: hay que ser transparente en la
  carta de presentación y en los changelogs sobre qué herramientas se usaron,
  qué partes afectaron y cómo se probó. Los mantenedores pueden tratarlo como
  cualquier contribución, revisarlo con más cuidado o rechazarlo.

Buena parte de esta preparación se hizo con ayuda de IA, así que la regla
aplica. El script no añade ninguna etiqueta por defecto. Para incluir un
trailer `Assisted-by:` (práctica habitual aunque ya no esté descrita en esos
documentos):
`ASSISTED_BY="AGENTE:MODELO" scripts/make-upstream-series.sh ...`, con el
nombre y la versión exactos de la herramienta usada. La carta de presentación (`docs/upstream/cover-letter.txt`) ya lleva el párrafo
de transparencia que pide `generated-content.rst`; ajústalo a lo que hiciste de
verdad antes de enviar. El `Signed-off-by` que
añade el script debe generarlo el humano que envía: ejecútalo tú, no una
herramienta de IA.

## Calendario realista

- **Ritmo:** sale un kernel cada 9 semanas (10 si cae en vacaciones). Últimas
  versiones: 7.1 el 14 de junio de 2026, 7.2 el 16 de agosto y 7.3 el 18 de
  octubre. 7.4-rc1 salió a primeros de noviembre de 2026; drm-misc-next ya acumula lo que irá en 7.5.
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

Si el árbol ya existe (`~/linux-drm-misc`, clon superficial), ponerlo al día y
**volver a `drm-misc-next` antes de regenerar**, o la serie nueva se apila
encima de la vieja:

```bash
cd ~/linux-drm-misc
git checkout drm-misc-next
git fetch --depth=1 origin drm-misc-next
git reset --hard origin/drm-misc-next
make olddefconfig          # el .config ya tiene CONFIG_DRM_FL2000=m
```

## 2. Generar la serie

```bash
cd ~/Projects/fl2000_drm_kernel7.1
CHECK_BUILD=1 scripts/make-upstream-series.sh ~/linux-drm-misc   # deja los .patch en ./outgoing
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

En Arch, `git send-email` necesita además `sudo pacman -S perl-authen-sasl
perl-io-socket-ssl perl-mime-tools`. Gmail requiere una *contraseña de aplicación* (Cuenta de Google → Seguridad →
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
2. ~~**Bridge sin conector propio**~~ **hecho** (5 nov 2026, falta probarlo en
   hardware): el IT66121 se engancha con `DRM_BRIDGE_ATTACH_NO_CONNECTOR`, el
   conector HDMI sale de `drm_bridge_connector_init()` y el modo se programa en
   `atomic_enable`, como en `ite-it66121`. El kernel de Arch no trae
   `CONFIG_DRM_BRIDGE_CONNECTOR`; para compilar fuera del árbol se meten en el
   módulo las fuentes oficiales de `compat/drm-helpers/` (ver `compat/README`).
3. **Infoframes a mano**: lo moderno es `DRM_BRIDGE_OP_HDMI`. Ya no queda
   `mode_set` en el bridge.
4. ~~**Estado compartido en el `struct usb_device`**~~,
5. ~~**Framework de componentes**~~,
6. ~~**El dispositivo DRM cuelga del adaptador I²C**~~ y
7. ~~**Una interfaz y no tres**~~: **hechos** el 6 nov 2026, de una vez, a raíz de
   la revisión de Sashiko: se enlaza la interfaz 0, se reclaman las otras dos con
   `usb_driver_claim_interface()`, todo cuelga de ese enlace, el bridge se engancha
   con `drm_bridge_attach()` y el dispositivo DRM cuelga de la interfaz, con
   `drm_dev_set_dma_dev(drm, usb_intf_get_dma_device(intf))`. Probado en una VM con
   KASAN y lockdep, falta el adaptador.
8. ~~**Suspend/resume real**~~ **hecho** el 5 nov 2026 (`drm_mode_config_helper_suspend()`/`_resume()`, también `reset_resume`), falta probarlo en hardware. El ancho de banda (RGB888/RGB565) se corrigió el mismo día.
