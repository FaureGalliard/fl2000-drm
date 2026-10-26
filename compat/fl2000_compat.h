/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Out-of-tree build only, never part of the upstream patches: lets the
 * driver, which follows the current DRM API (drm-misc-next / 7.4 cycle),
 * build on older kernels. The top-level Makefile force-includes this file
 * and defines the FL2000_COMPAT_* switches after probing the target
 * kernel's headers.
 */
#ifndef FL2000_COMPAT_H
#define FL2000_COMPAT_H

/* struct drm_atomic_commit was called struct drm_atomic_state */
#ifdef FL2000_COMPAT_ATOMIC_STATE
#define drm_atomic_commit drm_atomic_state
#endif

/* drm_bridge_funcs.atomic_create_state was called atomic_reset */
#ifdef FL2000_COMPAT_BRIDGE_RESET
#define atomic_create_state atomic_reset
#define drm_atomic_helper_bridge_create_state drm_atomic_helper_bridge_reset
#endif

#endif
