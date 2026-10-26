# Comparación con los otros drivers FL2000 (26 de octubre de 2026)

Revisión de las versiones más recientes de los demás proyectos activos,
hecha para ver qué se puede aprender o adoptar de cada uno. Los datos
salen de leer el código de cada repositorio en esa fecha.

| | **Este fork** | **klogg** (Artem Mygaiev) | **rhgndf** (Jie Feng) | **Samuv5** | **vfox888-cpu/alpha-fl2000** |
|---|---|---|---|---|---|
| Último commit | 26-oct | 19-oct | 15-sep (PR #86 a klogg) | 21-oct | 23-oct |
| Origen | klogg → Samuv5 (versión de mayo) → arreglos propios | original, reescrito en octubre | fork de klogg | reescritura desde cero (20-oct) | desde cero |
| Tamaño (C) | ~3.500 líneas | ~11.300 (incluye helpers del kernel copiados) | ~3.900 | ~3.050 | ~1.650 (un solo archivo) |
| Kernel objetivo | **7.1 y master (7.4)** | 7.0 | 7.2 | 7.2, 7.0, 6.18 | 7.0 |
| HDMI (IT66121) probado en hardware | **sí**, UGREEN + LG 1080p | sí, un adaptador, vía QEMU/KVM | no indicado | **no** (según su README) | sí, Hagibis, 1080p50–75 |
| VGA | no | no | **sí** | **sí** (detección por carga) | no |
| EDID por el bus I²C del FL2000 (0x50) | **sí** (desde jul-2026) | no (solo DDC del IT66121) | conector VGA con DDC | no | **sí** (desde oct-2026) |
| `simple_display_pipe` (obsoleto) | quitado | quitado | quitado | no lo usa | no lo usa |
| Bridge sin conector (`ATTACH_NO_CONNECTOR`) | no | sí | sí | — | — |
| `usb_driver_claim_interface` (una interfaz reclama las otras) | no | sí | sí | no | no |
| Helpers de conversión `drm_fb_xrgb8888_to_*` | **sí** | no (SIMD propio) | **sí** | no (propio, con test) | no |
| fbdev (`drm_client_setup`) | sí | sí | sí | sí | sí |
| DMA device (`drm_dev_set_dma_dev`) | no | sí | no | sí | sí |
| Suspend/resume real | no | sí | sí | sí | — |
| Vblank por temporizador de DRM | **sí** | no | no | no | no |
| RGB565 según ancho de banda | roto (unidades) | sí | — | parámetro `max_bpp` | — |
| Compila en Linus master (7.4) | **sí** | no (usa API vieja) | no verificado | no (usa API vieja) | no (usa API vieja) |
| Empaquetado | install.sh | DKMS + paquetes Debian | DKMS | DKMS + PKGBUILD (Arch) | DKMS + instalador |
| Pruebas | field test en hardware | tests de ciclo de vida, QEMU, benchmarks | — | test de conversión | — |
| Planes upstream | RFC preparado | dice que enviará el IT66121 "por separado" | PR a klogg "para estándares upstream" | no | no |

## Lo que solo tiene este fork

- **EDID por el bus I²C del FL2000, probado en hardware desde julio de 2026.**
  En tu adaptador es el único camino que funciona. klogg y Samuv5 solo usan el
  DDC del IT66121, que en este cableado nunca termina. vfox888 llegó a la
  misma solución por su cuenta en octubre.
- **Compila en el kernel en desarrollo (7.4).** En el ciclo 7.4 se quitaron
  los callbacks antiguos de los bridges y se renombró `drm_atomic_state`. Los
  demás proyectos usan esa API vieja y van a dejar de compilar.
- **Serie de parches lista** (`scripts/make-upstream-series.sh`): 4 parches,
  cada uno compila por separado, pasa checkpatch y lleva su carta de
  presentación.
- **Documentación de los bugs de fondo:** el ajuste de htotal que nunca se
  ejecutaba, la decodificación de interrupciones del IT66121 que no hacía
  nada, el ancho de banda medido en kHz como si fueran Hz y el choque de
  nombre con el driver `ite-it66121`.

## Qué conviene adoptar (en este orden, probando cada paso en el adaptador)

1. ~~Quitar `simple_display_pipe`~~ **hecho**.
2. ~~`drm_fb_xrgb8888_to_rgb888/565()` + intercambio de palabras sobre el
   búfer completo~~ **hecho** (idea del commit "Fix fl2000 specific dword
   ordering" de rhgndf). Arregla el bug de anchos raros, como 1366 px.
3. **El arreglo del signo en el ajuste de htotal**: rhgndf lo encontró, y aquí
   está en `docs/pending/`.
4. **Bridge con `DRM_BRIDGE_ATTACH_NO_CONNECTOR` + `drm_bridge_connector`**.
   El EDID del bus del FL2000 se puede ofrecer por `.edid_read` del bridge.
5. **Una sola interfaz con `usb_driver_claim_interface()`**, el dispositivo DRM
   colgado de la interfaz USB y `drm_dev_set_dma_dev()`.
6. ~~fbdev~~ **hecho**; faltan **suspend/resume y RGB565 por ancho de banda**.
7. **VGA** (rhgndf, Samuv5): amplía el hardware soportado.

## Atribución

Si se adopta código o ideas de otro proyecto, se reconoce en el mensaje del
commit, o con `Co-developed-by:` si el código es suyo. Para el kernel,
`Co-developed-by:` necesita además el `Signed-off-by:` de esa persona, así que
hay que pedírselo. Todos estos proyectos son GPL-2.0, igual que este.
