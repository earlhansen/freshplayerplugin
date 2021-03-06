/*
 * Copyright © 2013-2014  Rinat Ibragimov
 *
 * This file is part of FreshPlayerPlugin.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "tables.h"
#include "trace.h"
#include "ppb_var.h"
#include "p2n_proxy_class.h"
#include "n2p_proxy_class.h"
#include "config.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>


NPNetscapeFuncs     npn;
struct display_s    display;

static GHashTable  *pp_to_np_ht;
static GHashTable  *npobj_to_npp_ht = NULL;     // NPObject-to-NPP mapping

static PangoContext *pango_ctx = NULL;
static PangoFontMap *pango_fm = NULL;

static pthread_mutex_t  lock;
static int urandom_fd = -1;

static
void
__attribute__((constructor))
constructor_tables(void)
{
    // hash tables
    pp_to_np_ht =       g_hash_table_new(g_direct_hash, g_direct_equal);
    npobj_to_npp_ht =   g_hash_table_new(g_direct_hash, g_direct_equal);

    // pango
    pango_fm = pango_ft2_font_map_new();
    pango_ctx = pango_font_map_create_context(pango_fm);

    // mutex
    pthread_mutex_init(&lock, NULL);

    // urandom
    urandom_fd = open("/dev/urandom", O_RDONLY);
    srand(time(NULL) + 42);
}

static
void
__attribute__((destructor))
destructor_tables(void)
{
    // hash tables
    g_hash_table_unref(pp_to_np_ht);
    g_hash_table_unref(npobj_to_npp_ht);

    // pango
    g_object_unref(pango_ctx);
    g_object_unref(pango_fm);
    pango_ctx = NULL;
    pango_fm = NULL;

    // mutex
    pthread_mutex_destroy(&lock);

    // urandom
    close(urandom_fd);
}

int
tables_get_urandom_fd(void)
{
    return urandom_fd;
}

struct pp_instance_s *
tables_get_pp_instance(PP_Instance instance)
{
    pthread_mutex_lock(&lock);
    struct pp_instance_s *pp_i = g_hash_table_lookup(pp_to_np_ht, GINT_TO_POINTER(instance));
    pthread_mutex_unlock(&lock);
    return pp_i;
}

void
tables_add_pp_instance(PP_Instance instance, struct pp_instance_s *pp_i)
{
    pthread_mutex_lock(&lock);
    g_hash_table_replace(pp_to_np_ht, GINT_TO_POINTER(instance), pp_i);
    pthread_mutex_unlock(&lock);
}

void
tables_remove_pp_instance(PP_Instance instance)
{
    pthread_mutex_lock(&lock);
    g_hash_table_remove(pp_to_np_ht, GINT_TO_POINTER(instance));
    pthread_mutex_unlock(&lock);
}

struct pp_instance_s *
tables_get_some_pp_instance(void)
{
    GHashTableIter iter;
    gpointer key, value;

    pthread_mutex_lock(&lock);
    g_hash_table_iter_init (&iter, pp_to_np_ht);
    if (!g_hash_table_iter_next(&iter, &key, &value))
        value = NULL;
    pthread_mutex_unlock(&lock);

    return value;
}

PangoContext *
tables_get_pango_ctx(void)
{
    return pango_ctx;
}

PangoFontMap *
tables_get_pango_font_map(void)
{
    return pango_fm;
}

PangoFontDescription *
pp_font_desc_to_pango_font_desc(const struct PP_BrowserFont_Trusted_Description *description)
{
    PangoFontDescription *font_desc;

    if (description->face.type == PP_VARTYPE_STRING) {
        const char *s = ppb_var_var_to_utf8(description->face, NULL);
        font_desc = pango_font_description_from_string(s);
    } else {
        font_desc = pango_font_description_new();
        switch (description->family) {
        case PP_BROWSERFONT_TRUSTED_FAMILY_SERIF:
            pango_font_description_set_family(font_desc, "serif");
            break;
        case PP_BROWSERFONT_TRUSTED_FAMILY_SANSSERIF:
            pango_font_description_set_family(font_desc, "sans-serif");
            break;
        case PP_BROWSERFONT_TRUSTED_FAMILY_MONOSPACE:
            pango_font_description_set_family(font_desc, "monospace");
            break;
        case PP_BROWSERFONT_TRUSTED_FAMILY_DEFAULT:
            // fall through
        default:
            // do nothing
            break;
        }
    }

    pango_font_description_set_absolute_size(font_desc, description->size * PANGO_SCALE);
    pango_font_description_set_weight(font_desc, (description->weight + 1) * 100);
    if (description->italic)
        pango_font_description_set_style(font_desc, PANGO_STYLE_ITALIC);
    if (description->small_caps)
        pango_font_description_set_variant(font_desc, PANGO_VARIANT_SMALL_CAPS);

    return font_desc;
}

void
tables_add_npobj_npp_mapping(NPObject *npobj, NPP npp)
{
    pthread_mutex_lock(&lock);
    g_hash_table_insert(npobj_to_npp_ht, npobj, npp);
    pthread_mutex_unlock(&lock);
}

NPP
tables_get_npobj_npp_mapping(NPObject *npobj)
{
    pthread_mutex_lock(&lock);
    NPP npp = g_hash_table_lookup(npobj_to_npp_ht, npobj);
    pthread_mutex_unlock(&lock);
    return npp;
}

void
tables_remove_npobj_npp_mapping(NPObject *npobj)
{
    pthread_mutex_lock(&lock);
    g_hash_table_remove(npobj_to_npp_ht, npobj);
    pthread_mutex_unlock(&lock);
}

int
tables_open_display(void)
{
    EGLint major, minor;
    int retval = 0;

    pthread_mutex_init(&display.lock, NULL);
    pthread_mutex_lock(&display.lock);
    display.x = XOpenDisplay(NULL);
    if (!display.x) {
        trace_error("%s, can't open X Display\n", __func__);
        retval = 1;
        goto quit;
    }

    if (config.quirks.x_synchronize)
        XSynchronize(display.x, True);

    display.egl = eglGetDisplay(display.x);
    eglInitialize(display.egl, &major, &minor);
    trace_info_f("EGL version %d.%d\n", major, minor);

    // get fullscreen resolution
    XWindowAttributes xw_attrs;
    if (XGetWindowAttributes(display.x, DefaultRootWindow(display.x), &xw_attrs)) {
        display.fs_width =  xw_attrs.width;
        display.fs_height = xw_attrs.height;
    } else {
        display.fs_width = 100;
        display.fs_height = 100;
    }

    // create transparent cursor
    const char t_pixmap_data = 0;
    XColor t_color = {};
    Pixmap t_pixmap = XCreateBitmapFromData(display.x, DefaultRootWindow(display.x),
                                            &t_pixmap_data, 1, 1);
    display.transparent_cursor = XCreatePixmapCursor(display.x, t_pixmap, t_pixmap, &t_color,
                                                     &t_color, 0, 0);
    XFreePixmap(display.x, t_pixmap);

quit:
    pthread_mutex_unlock(&display.lock);
    return retval;
}

void
tables_close_display(void)
{
    pthread_mutex_lock(&display.lock);
    XFreeCursor(display.x, display.transparent_cursor);
    eglTerminate(display.egl);
    XCloseDisplay(display.x);
    pthread_mutex_unlock(&display.lock);
    pthread_mutex_destroy(&display.lock);
}
