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

#define GL_GLEXT_PROTOTYPES
#include <assert.h>
#include <inttypes.h>
#include <npapi/npapi.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include "trace.h"
#include "reverse_constant.h"
#include "pp_interface.h"
#include "pp_resource.h"
#include "tables.h"
#include "config.h"
#include "p2n_proxy_class.h"
#include <ppapi/c/ppp_instance.h>
#include <ppapi/c/ppp_input_event.h>
#include <ppapi/c/pp_errors.h>
#include <ppapi/c/private/ppp_instance_private.h>
#include "ppb_input_event.h"
#include "ppb_url_loader.h"
#include "ppb_url_request_info.h"
#include "ppb_var.h"
#include "ppb_core.h"
#include "ppb_message_loop.h"
#include "header_parser.h"
#include "keycodeconvert.h"
#include "eintr_retry.h"
#include "main_thread.h"


static
PP_Instance
generate_new_pp_instance_id(void)
{
    static int32_t instance_id = 10;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&m);
    int32_t result = instance_id++;
    pthread_mutex_unlock(&m);

    return result;
}

static
void
do_nothing(void *user_data, int32_t result)
{
    (void)user_data;
    (void)result;
}

static
void
_set_window_comt(void *user_data, int32_t result)
{
    struct pp_instance_s *pp_i = user_data;
    PP_Resource view = pp_resource_allocate(PP_RESOURCE_VIEW, pp_i);
    struct pp_view_s *v = pp_resource_acquire(view, PP_RESOURCE_VIEW);

    if (v) {
        pthread_mutex_lock(&display.lock);
        v->rect.point.x = 0; // TODO: pp_i->x;
        v->rect.point.y = 0; // TODO: pp_i->y;
        v->rect.size.width = pp_i->width;
        v->rect.size.height = pp_i->height;
        pp_resource_release(view);
        pthread_mutex_unlock(&display.lock);

        pp_i->ppp_instance_1_1->DidChangeView(pp_i->id, view);
        ppb_core_release_resource(view);
    }
}

NPError
NPP_SetWindow(NPP npp, NPWindow *window)
{
    if (config.quirks.plugin_missing)
        return NPERR_NO_ERROR;

    char *window_str = trace_np_window_as_string(window);
    trace_info_f("[NPP] {full} %s npp=%p, window=%s\n", __func__, npp, window_str);
    g_free(window_str);

    struct pp_instance_s *pp_i = npp->pdata;
    if (!pp_i) {
        trace_error("%s, pp_i is NULL\n", __func__);
        return NPERR_NO_ERROR;
    }

    pthread_mutex_lock(&display.lock);
    if (pp_i && !pp_i->is_fullscreen) {
        pp_i->wnd = (Window)window->window;
        pp_i->width = window->width;
        pp_i->height = window->height;

        if (g_atomic_int_get(&pp_i->instance_loaded))
            ppb_core_call_on_main_thread(0, PP_MakeCCB(_set_window_comt, pp_i), PP_OK);
    }
    pthread_mutex_unlock(&display.lock);

    return NPERR_NO_ERROR;
}

struct call_plugin_did_create_param_s {
    PP_Resource             m_loop;
    int                     depth;
    struct pp_instance_s   *pp_i;
};

static
void
_call_plugin_did_create_comt(void *user_data, int32_t result)
{
    struct call_plugin_did_create_param_s *p = user_data;
    struct pp_instance_s *pp_i = p->pp_i;

    pp_i->ppp_instance_1_1->DidCreate(pp_i->id, pp_i->argc, (const char **)pp_i->argn,
                                      (const char **)pp_i->argv);

    // no need to keep argn/argv after initialization
    for (uintptr_t k = 0; k < pp_i->argc; k++) {
        free(pp_i->argn[k]);
        free(pp_i->argv[k]);
    }
    free_and_nullify(pp_i->argn);
    free_and_nullify(pp_i->argv);

    pp_i->ppp_instance_private = ppp_get_interface(PPP_INSTANCE_PRIVATE_INTERFACE_0_1);
    if (pp_i->ppp_instance_private && pp_i->ppp_instance_private->GetInstanceObject) {
        struct PP_Var ppobj = pp_i->ppp_instance_private->GetInstanceObject(pp_i->id);
        NPVariant np_var = pp_var_to_np_variant(ppobj);
        ppb_var_release(ppobj);

        pp_i->scriptable_obj = np_var.value.objectValue;
        tables_add_npobj_npp_mapping(np_var.value.objectValue, pp_i->npp);
    }

    if (pp_i->is_fullframe) {
        PP_Resource request_info = ppb_url_request_info_create(pp_i->id);
        PP_Resource url_loader = ppb_url_loader_create(pp_i->id);

        struct PP_Var s_method = ppb_var_var_from_utf8_z("GET");
        ppb_url_request_info_set_property(request_info, PP_URLREQUESTPROPERTY_URL,
                                          pp_i->instance_url);
        ppb_url_request_info_set_property(request_info, PP_URLREQUESTPROPERTY_METHOD, s_method);
        ppb_url_loader_open(url_loader, request_info, PP_MakeCCB(do_nothing, NULL));
        ppb_var_release(s_method);
        ppb_core_release_resource(request_info);

        pp_i->ppp_instance_1_1->HandleDocumentLoad(pp_i->id, url_loader);
    }

    g_atomic_int_set(&pp_i->instance_loaded, 1);

    ppb_message_loop_post_quit_depth(p->m_loop, PP_FALSE, p->depth);
}

static
struct PP_Var
get_document_url(const struct pp_instance_s *pp_i)
{
    struct PP_Var document_url = PP_MakeUndefined();
    NPIdentifier location_id = npn.getstringidentifier("location");
    NPIdentifier href_id = npn.getstringidentifier("href");
    NPObject *np_location_obj;
    NPVariant location_var, href_var;

    if (!npn.getproperty(pp_i->npp, pp_i->np_window_obj, location_id, &location_var))
        goto err_2;

    if (location_var.type != NPVariantType_Object)
        goto err_3;

    np_location_obj = location_var.value.objectValue;
    if (!npn.getproperty(pp_i->npp, np_location_obj, href_id, &href_var))
        goto err_3;

    struct PP_Var var = np_variant_to_pp_var(href_var);
    if (var.type != PP_VARTYPE_STRING) {
        ppb_var_release(var);
        goto err_4;
    }

    document_url = var;

err_4:
    npn.releasevariantvalue(&href_var);
err_3:
    npn.releasevariantvalue(&location_var);
err_2:
    return document_url;
}

NPError
NPP_New(NPMIMEType pluginType, NPP npp, uint16_t mode, int16_t argc, char *argn[],
        char *argv[], NPSavedData *saved)
{
    int k;
    struct pp_instance_s *pp_i;
    trace_info_f("[NPP] {full} %s pluginType=%s npp=%p, mode=%d, argc=%d, saved=%p\n", __func__,
                 pluginType, npp, mode, argc, saved);

    for (k = 0; k < argc; k ++)
        trace_info_f("            argn[%d] = %s, argv[%d] = %s\n", k, argn[k], k, argv[k]);

    // request windowless operation
    npn.setvalue(npp, NPPVpluginWindowBool, (void*)0);

    if (config.quirks.plugin_missing) {
        trace_info_z("plugin missing, using placeholder\n");
        return NPERR_NO_ERROR;
    }

    pp_i = calloc(sizeof(*pp_i), 1);
    npp->pdata = pp_i;
    if (!pp_i)
        return NPERR_OUT_OF_MEMORY_ERROR;

    pthread_mutex_lock(&display.lock);
    pp_i->npp = npp;
    pthread_mutex_unlock(&display.lock);

    pp_i->ppp_instance_1_1 = ppp_get_interface(PPP_INSTANCE_INTERFACE_1_1);
    if (!pp_i->ppp_instance_1_1)
        return NPERR_GENERIC_ERROR;
    pp_i->ppp_input_event = ppp_get_interface(PPP_INPUT_EVENT_INTERFACE_0_1);

    pp_i->argc = argc;
    pp_i->argn = malloc(argc * sizeof(char*));
    pp_i->argv = malloc(argc * sizeof(char*));
    for (k = 0; k < argc; k ++) {
        pp_i->argn[k] = strdup(argn[k] ? argn[k] : "");
        pp_i->argv[k] = strdup(argv[k] ? argv[k] : "");

        if (strcasecmp(argn[k], "src") == 0) {
            pp_i->instance_url = ppb_var_var_from_utf8_z(argv[k]);
        }

        if (strcasecmp(argn[k], "wmode") == 0)
            if (strcasecmp(argv[k], "transparent") == 0)
                pp_i->is_transparent = 1;
    }

    // set transparency mode
    npn.setvalue(npp, NPPVpluginTransparentBool, (void*)(size_t)pp_i->is_transparent);

    pp_i->is_fullframe = (mode == NP_FULL);
    pp_i->id = generate_new_pp_instance_id();
    tables_add_pp_instance(pp_i->id, pp_i);

    pp_i->incognito_mode = 0;
    if (npn.version >= NPVERS_HAS_PRIVATE_MODE) {
        NPBool private = false;
        if (npn.getvalue(pp_i->npp, NPNVprivateModeBool, &private) == NPERR_NO_ERROR)
            pp_i->incognito_mode = private ? 1 : 0;
    }

    do {
        // getting window object
        NPError err = npn.getvalue(npp, NPNVWindowNPObject, &pp_i->np_window_obj);
        if (err == NPERR_NO_ERROR) {
            tables_add_npobj_npp_mapping(pp_i->np_window_obj, npp);
        } else {
            trace_error("%s, failed to get NPNVWindowNPObject, err = %d\n", __func__, err);
            pp_i->np_window_obj = NULL;
        }

        // getting plugin element object
        err = npn.getvalue(npp, NPNVPluginElementNPObject, &pp_i->np_plugin_element_obj);
        if (err == NPERR_NO_ERROR) {
            tables_add_npobj_npp_mapping(pp_i->np_plugin_element_obj, npp);
        } else {
            trace_error("%s, failed to get NPNVPluginElementNPObject, err = %d\n", __func__, err);
            pp_i->np_plugin_element_obj = NULL;
        }
    } while (0);

    pp_i->document_url = get_document_url(pp_i);

    if (ppb_message_loop_get_current() == 0) {
        // allocate message loop for browser thread
        PP_Resource message_loop = ppb_message_loop_create(pp_i->id);
        ppb_message_loop_attach_to_current_thread(message_loop);
        ppb_message_loop_proclaim_this_thread_browser();
    }

    if (ppb_message_loop_get_for_main_thread() == 0) {
        pthread_barrier_init(&pp_i->main_thread_barrier, NULL, 2);
        pthread_create(&pp_i->main_thread, NULL, fresh_wrapper_main_thread, pp_i);
        pthread_detach(pp_i->main_thread);
        pthread_barrier_wait(&pp_i->main_thread_barrier);
        pthread_barrier_destroy(&pp_i->main_thread_barrier);
    }

    struct call_plugin_did_create_param_s *p = g_slice_alloc(sizeof(*p));
    p->m_loop = ppb_message_loop_get_for_browser_thread();
    p->depth =  ppb_message_loop_get_depth(p->m_loop) + 1;
    p->pp_i =   pp_i;
    ppb_core_call_on_main_thread(0, PP_MakeCCB(_call_plugin_did_create_comt, p), PP_OK);
    ppb_message_loop_run_nested(p->m_loop);
    g_slice_free1(sizeof(*p), p);

    return NPERR_NO_ERROR;
}

struct destroy_instance_param_s {
    struct pp_instance_s   *pp_i;
    PP_Resource             m_loop;
    int                     depth;
};

static
void
_destroy_instance_comt(void *user_data, int32_t result)
{
    struct destroy_instance_param_s *p = user_data;

    p->pp_i->ppp_instance_1_1->DidDestroy(p->pp_i->id);
    tables_remove_pp_instance(p->pp_i->id);
    pthread_mutex_lock(&display.lock);
    p->pp_i->npp = NULL;
    pthread_mutex_unlock(&display.lock);

    npn.releaseobject(p->pp_i->np_window_obj);
    npn.releaseobject(p->pp_i->np_plugin_element_obj);
    npn.releaseobject(p->pp_i->scriptable_obj);

    tables_remove_npobj_npp_mapping(p->pp_i->np_window_obj);
    tables_remove_npobj_npp_mapping(p->pp_i->np_plugin_element_obj);
    tables_remove_npobj_npp_mapping(p->pp_i->scriptable_obj);

    ppb_var_release(p->pp_i->instance_url);
    ppb_var_release(p->pp_i->document_url);
    free(p->pp_i);
    ppb_message_loop_post_quit_depth(p->m_loop, PP_FALSE, p->depth);
}

NPError
NPP_Destroy(NPP npp, NPSavedData **save)
{
    trace_info_f("[NPP] {full} %s npp=%p, save=%p\n", __func__, npp, save);
    struct pp_instance_s *pp_i = npp->pdata;

    if (config.quirks.plugin_missing)
        return NPERR_NO_ERROR;

    if (pp_i->have_prev_cursor) {
        pthread_mutex_lock(&display.lock);
        XFreeCursor(display.x, pp_i->prev_cursor);
        pthread_mutex_unlock(&display.lock);
    }

    struct destroy_instance_param_s *p = g_slice_alloc(sizeof(*p));
    p->pp_i =   npp->pdata;
    p->m_loop = ppb_message_loop_get_current();
    p->depth =  ppb_message_loop_get_depth(p->m_loop) + 1;

    ppb_core_call_on_main_thread(0, PP_MakeCCB(_destroy_instance_comt, p), PP_OK);
    ppb_message_loop_run_nested(p->m_loop);
    g_slice_free1(sizeof(*p), p);

    if (save)
        *save = NULL;
    return NPERR_NO_ERROR;
}

NPError
NPP_NewStream(NPP npp, NPMIMEType type, NPStream *stream, NPBool seekable, uint16_t *stype)
{
    trace_info_f("[NPP] {full} %s npp=%p, type=%s, stream={.pdata=%p, .ndata=%p, .url=%s, "
                 "end=%u, lastmodified=%u, .notifyData=%u, .headers=%s}, seekable=%d\n", __func__,
                 npp, type, stream->pdata, stream->ndata, stream->url, stream->end,
                 stream->lastmodified, (unsigned)(size_t)stream->notifyData, stream->headers,
                 seekable);

    if (config.quirks.plugin_missing)
        return NPERR_NO_ERROR;

    PP_Resource loader = (size_t)stream->notifyData;
    if (!loader) {
        // ignoring unrequested streams
        stream->pdata = NULL;
        trace_info_f("      ignoring unrequested stream\n");
        return NPERR_NO_ERROR;
    }

    struct PP_CompletionCallback ccb = {};

    stream->pdata = (void*)(size_t)loader;
    struct pp_url_loader_s *ul = pp_resource_acquire(loader, PP_RESOURCE_URL_LOADER);

    if (ul) {
        struct parsed_headers_s *ph = hp_parse_headers(stream->headers);
        ccb = ul->ccb;
        ul->np_stream = stream;

        // handling redirection
        if (ph->http_code >= 300 && ph->http_code <= 307 && ul->redirect_url) {
            if (ul->follow_redirects) {
                trace_info_f("       %s, redirecting to %s\n", __func__, ul->redirect_url);
                pp_resource_release(loader);
                ppb_url_loader_follow_redirect(loader, PP_MakeCCB(do_nothing, NULL));
                // There is no need to fill response headers, status_line and other parameters
                // since they are freed in follow_redirect anyway.
                hp_free_parsed_headers(ph);
                goto quit;
            }
        }

        size_t headers_len = 0;
        for (uintptr_t k = 0; k < ph->cnt; k ++)
            headers_len += strlen(ph->name[k]) + strlen(": ") + strlen(ph->value[k]) + strlen("\n");

        // reconstruct headers
        ul->headers = malloc(headers_len + 1);
        char *ptr = ul->headers;
        for (unsigned int k = 0; k < ph->cnt; k ++) {
            if (k != 0) {
                memcpy(ptr, "\n", strlen("\n"));
                ptr += strlen("\n");
            }
            memcpy(ptr, ph->name[k], strlen(ph->name[k]));  ptr += strlen(ph->name[k]);
            memcpy(ptr, ": ", strlen(": "));                ptr += strlen(": ");
            memcpy(ptr, ph->value[k], strlen(ph->value[k]));ptr += strlen(ph->value[k]);
        }
        *ptr = 0;
        ul->http_code = ph->http_code;
        ul->response_size = stream->end > 0 ? stream->end : -1;
        ul->status_line = nullsafe_strdup(ph->status_line);

        hp_free_parsed_headers(ph);
        pp_resource_release(loader);
    }

quit:
    if (ccb.func)
        ppb_core_call_on_main_thread(0, ccb, PP_OK);

    return NPERR_NO_ERROR;
}

NPError
NPP_DestroyStream(NPP npp, NPStream *stream, NPReason reason)
{
    trace_info_f("[NPP] {full} %s npp=%p, stream={.pdata=%p, .ndata=%p, .url=%s, end=%u, "
                 "lastmodified=%u, .notifyData=%u, .headers=%s}, reason=%d\n", __func__,
                 npp, stream->pdata, stream->ndata, stream->url, stream->end,
                 stream->lastmodified, (unsigned)(size_t)stream->notifyData, "<skipped>", reason);

    if (config.quirks.plugin_missing)
        return NPERR_NO_ERROR;

    PP_Resource loader = (PP_Resource)(size_t)stream->pdata;
    if (!loader)
        return NPERR_NO_ERROR;

    struct pp_url_loader_s *ul = pp_resource_acquire(loader, PP_RESOURCE_URL_LOADER);
    if (!ul)
        return NPERR_NO_ERROR;

    if (ul->redirect_url) {
        pp_resource_release(loader);
        return NPERR_NO_ERROR;
    }

    ul->finished_loading = 1;

    // execute all remaining tasks in task list
    while (ul && ul->read_tasks) {
        GList *llink = g_list_first(ul->read_tasks);
        struct url_loader_read_task_s *rt = llink->data;
        ul->read_tasks = g_list_delete_link(ul->read_tasks, llink);

        int32_t read_bytes = -1;
        off_t ofs = lseek(ul->fd, ul->read_pos, SEEK_SET);
        if (ofs != (off_t) -1)
            read_bytes = RETRY_ON_EINTR(read(ul->fd, rt->buffer, rt->bytes_to_read));

        if (read_bytes == -1)
            read_bytes = PP_ERROR_FAILED;
        else
            ul->read_pos += read_bytes;

        pp_resource_release(loader);
        ppb_core_call_on_main_thread(0, rt->ccb, read_bytes);
        g_slice_free(struct url_loader_read_task_s, rt);
        ul = pp_resource_acquire(loader, PP_RESOURCE_URL_LOADER);
    }

    if (ul && ul->stream_to_file) {
        struct PP_CompletionCallback ccb = ul->stream_to_file_ccb;

        pp_resource_release(loader);
        ppb_core_call_on_main_thread(0, ccb, PP_OK);
        return NPERR_NO_ERROR;
    }

    pp_resource_release(loader);
    return NPERR_NO_ERROR;
}

int32_t
NPP_WriteReady(NPP npp, NPStream *stream)
{
    trace_info_f("[NPP] {full} %s npp=%p, stream=%p\n", __func__, npp, stream);
    return 1024*1024;
}

int32_t
NPP_Write(NPP npp, NPStream *stream, int32_t offset, int32_t len, void *buffer)
{
    trace_info_f("[NPP] {full} %s npp=%p, stream={.pdata=%p, .ndata=%p, .url=%s, end=%u, "
                 "lastmodified=%u, .notifyData=%u, .headers=<skipped>}, offset=%d, len=%d, "
                 "buffer=%p\n",
                 __func__, npp, stream->pdata, stream->ndata, stream->url, stream->end,
                 stream->lastmodified, (unsigned)(size_t)stream->notifyData, offset, len, buffer);
    if (config.quirks.plugin_missing)
        return len;

    PP_Resource loader = (PP_Resource)(size_t)stream->pdata;
    if (!loader)
        return len;

    struct pp_url_loader_s *ul = pp_resource_acquire(loader, PP_RESOURCE_URL_LOADER);
    if (!ul) {
        trace_info_f("[NPP] %s, ignoring stream content\n", __func__);
        return -1;
    }

    if (ul->fd == -1 || len <= 0) {
        pp_resource_release(loader);
        return len;
    }

    if (lseek(ul->fd, offset, SEEK_SET) == (off_t) -1) {
        pp_resource_release(loader);
        return -1;
    }

    RETRY_ON_EINTR(write(ul->fd, buffer, len));

    if (ul->read_tasks == NULL) {
        pp_resource_release(loader);
        return len;
    }

    GList *llink = g_list_first(ul->read_tasks);
    struct url_loader_read_task_s *rt = llink->data;
    ul->read_tasks = g_list_delete_link(ul->read_tasks, llink);

    int32_t read_bytes = -1;
    off_t ofs = lseek(ul->fd, ul->read_pos, SEEK_SET);
    if (ofs != (off_t) -1)
        read_bytes = RETRY_ON_EINTR(read(ul->fd, rt->buffer, rt->bytes_to_read));
    if (read_bytes > 0)
        ul->read_pos += read_bytes;

    if (read_bytes > 0) {
        pp_resource_release(loader);
        ppb_core_call_on_main_thread(0, rt->ccb, read_bytes);
        g_slice_free(struct url_loader_read_task_s, rt);
        return len;
    } else {
        // reschedule task
        ul->read_tasks = g_list_prepend(ul->read_tasks, rt);
        pp_resource_release(loader);
        return len;
    }
}

void
NPP_StreamAsFile(NPP npp, NPStream *stream, const char *fname)
{
    trace_info_z("[NPP] {zilch} %s npp=%p, stream=%p, fname=%s\n", __func__, npp, stream, fname);
    return;
}

void
NPP_Print(NPP npp, NPPrint *platformPrint)
{
    trace_info_z("[NPP] {zilch} %s npp=%p, platformPrint=%p\n", __func__, npp, platformPrint);
    return;
}

static
int16_t
handle_graphics_expose_event(NPP npp, void *event)
{
    XGraphicsExposeEvent *ev = event;
    struct pp_instance_s *pp_i = npp->pdata;
    struct pp_graphics2d_s *g2d = pp_resource_acquire(pp_i->graphics, PP_RESOURCE_GRAPHICS2D);
    struct pp_graphics3d_s *g3d = pp_resource_acquire(pp_i->graphics, PP_RESOURCE_GRAPHICS3D);
    Display *dpy = ev->display;
    Drawable drawable = ev->drawable;
    int screen = 0;
    cairo_surface_t *src_surf, *dst_surf;
    cairo_t *cr;
    int retval;

    pthread_mutex_lock(&display.lock);
    if (g2d) {
        if (pp_i->is_transparent) {
            XVisualInfo vi;
            struct {
                Window root;
                int x, y;
                unsigned int width, height, border, depth;
            } d = {};

            XGetGeometry(dpy, drawable, &d.root, &d.x, &d.y, &d.width, &d.height, &d.border,
                         &d.depth);
            if (XMatchVisualInfo(dpy, screen, d.depth, TrueColor, &vi)) {
                dst_surf = cairo_xlib_surface_create(dpy, drawable, vi.visual, d.width, d.height);
                src_surf = cairo_image_surface_create_for_data((unsigned char *)g2d->second_buffer,
                    CAIRO_FORMAT_ARGB32, g2d->scaled_width, g2d->scaled_height, g2d->scaled_stride);
                cr = cairo_create(dst_surf);
                cairo_set_source_surface(cr, src_surf, 0, 0);
                cairo_rectangle(cr, ev->x, ev->y, MIN(g2d->scaled_width, ev->width),
                                MIN(g2d->scaled_height, ev->height));
                cairo_fill(cr);
                cairo_destroy(cr);
                cairo_surface_destroy(dst_surf);
                cairo_surface_destroy(src_surf);
                XFlush(dpy);
            }
        } else {
            XImage *xi = XCreateImage(dpy, DefaultVisual(dpy, screen), 24, ZPixmap, 0,
                                      g2d->second_buffer, g2d->scaled_width, g2d->scaled_height, 32,
                                      g2d->scaled_stride);

            XPutImage(dpy, drawable, DefaultGC(dpy, screen), xi, 0, 0,
                      ev->x, ev->y,
                      MIN(g2d->scaled_width, ev->width), MIN(g2d->scaled_height, ev->height));
            XFree(xi);
        }
    } else if (g3d) {
        XSync(dpy, False);
        if (pp_i->is_transparent) {
            int ret;
            // copy background to own pixmap
            XCopyArea(dpy, drawable, g3d->pixmap, DefaultGC(dpy, screen),
                      ev->x, ev->y,
                      ev->width, ev->height,
                      ev->x, ev->y);
            XSync(dpy, False);

            eglWaitNative(EGL_CORE_NATIVE_ENGINE);
            ret = eglMakeCurrent(display.egl, g3d->egl_surf, g3d->egl_surf, g3d->glc_t);
            if (!ret) {
                trace_error("%s, eglMakeCurrent failed\n", __func__);
                // TODO: abort?
            }
            glViewport(0, 0, g3d->width, g3d->height);
            glBindTexture(GL_TEXTURE_2D, g3d->tex_back);
            glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, g3d->width, g3d->height, 0);

            GLfloat p_left =    (float)ev->x / g3d->width;
            GLfloat p_right =   (float)(ev->x + ev->width) / g3d->width;
            GLfloat p_top =     (float)(g3d->height - ev->y - ev->height) / g3d->height;
            GLfloat p_bottom =  (float)(g3d->height - ev->y) / g3d->height;
            GLfloat square_vertices[] = {
                p_left, p_top,
                p_right, p_top,
                p_left, p_bottom,
                p_right, p_bottom,
            };

            glUseProgram(g3d->prog.id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g3d->tex_back);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g3d->tex_front);

            glUniform1i(g3d->prog.uniform_tex_back, 0);
            glUniform1i(g3d->prog.uniform_tex_front, 1);

            glVertexAttribPointer(g3d->prog.attrib_pos, 2, GL_FLOAT, 0, 0, square_vertices);
            glEnableVertexAttribArray(g3d->prog.attrib_pos);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(g3d->prog.attrib_pos);

            glFinish();
            eglMakeCurrent(display.egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglWaitGL();
        }

        XCopyArea(dpy, g3d->pixmap, drawable, DefaultGC(dpy, screen),
                  ev->x, ev->y,
                  ev->width, ev->height,
                  ev->x, ev->y);
        XSync(dpy, False);
    } else {
        retval = 0;
        goto done;
    }

    pp_resource_release(pp_i->graphics);
    if (pp_i->graphics_in_progress) {
        if (pp_i->graphics_ccb.func) {
            ppb_core_call_on_main_thread(0, pp_i->graphics_ccb, PP_OK);
        } else {
            pthread_mutex_unlock(&display.lock);
            pthread_barrier_wait(&pp_i->graphics_barrier);
            pthread_mutex_lock(&display.lock);
        }
    }

    pp_i->graphics_in_progress = 0;
    retval = 1;

done:
    pthread_mutex_unlock(&display.lock);
    return retval;
}

/// diplay plugin placeholder and error message in it
static
int16_t
handle_placeholder_graphics_expose_event(NPP npp, void *event)
{
    XGraphicsExposeEvent   *ev = event;
    Display                *dpy = ev->display;
    Drawable                drawable = ev->drawable;
    int                     screen = 0;
    unsigned int            width, height, border_width, depth;
    Window                  root_wnd;
    int                     x, y;

    XGetGeometry(dpy, drawable, &root_wnd, &x, &y, &width, &height, &border_width, &depth);
    cairo_surface_t *xlib_surf = cairo_xlib_surface_create(dpy, drawable,
                                                           DefaultVisual(dpy, screen),
                                                           width, height);
    cairo_t *cr = cairo_create(xlib_surf);
    double bg_color[3] = {0.35, 0.35, 0.3};
    double fg_color[3] = {0.9, 0.9, 0.5};


    // clear box
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
    cairo_fill(cr);

    // draw crossed box
    cairo_set_source_rgb(cr, fg_color[0], fg_color[1], fg_color[2]);
    cairo_set_line_width(cr, 3);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, 0);        cairo_line_to(cr, width, height);
    cairo_move_to(cr, 0, height);   cairo_line_to(cr, width, 0);
    cairo_stroke(cr);

    // show error text
    cairo_text_extents_t extents;
    gchar *txt;

    if (config.quirks.incompatible_npapi_version) {
        txt = g_strdup_printf("NPAPI version too old (%d)", npn.version);
    } else {
        txt = g_strdup_printf("Failed to load \"%s\"", fpp_config_get_plugin_file_name());
    }
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, 10.0, 30.0);
    cairo_text_extents(cr, txt, &extents);

    // prepare background
    cairo_rectangle(cr, 10.0, 30.0, extents.width + 6, extents.height + 6);
    cairo_set_source_rgb(cr, bg_color[0], bg_color[1], bg_color[2]);
    cairo_fill(cr);

    // draw text itself
    cairo_set_source_rgb(cr, fg_color[0], fg_color[1], fg_color[2]);
    cairo_move_to(cr, 10.0 + 3, 30.0 + extents.height);
    cairo_show_text(cr, txt);
    g_free(txt);

    cairo_destroy(cr);
    cairo_surface_destroy(xlib_surf);
    return 1;
}

static
unsigned int
x_state_mask_to_pp_inputevent_modifier(unsigned int state)
{
    unsigned int mod = 0;

    // TODO: refine this
    if (state & ShiftMask) {
        mod |= PP_INPUTEVENT_MODIFIER_SHIFTKEY;
        mod |= PP_INPUTEVENT_MODIFIER_ISLEFT;
    }

    if (state & LockMask) {
        mod |= PP_INPUTEVENT_MODIFIER_CAPSLOCKKEY;
    }

    if (state & ControlMask) {
        mod |= PP_INPUTEVENT_MODIFIER_CONTROLKEY;
        mod |= PP_INPUTEVENT_MODIFIER_ISLEFT;
    }

    if (state & Mod1Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_ALTKEY;
        mod |= PP_INPUTEVENT_MODIFIER_ISLEFT;
    }

    if (state & Mod2Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_NUMLOCKKEY;
    }

    if (state & Mod4Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_METAKEY;
        mod |= PP_INPUTEVENT_MODIFIER_ISLEFT;
    }

    if (state & Button1Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_LEFTBUTTONDOWN;
    }

    if (state & Button2Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_MIDDLEBUTTONDOWN;
    }

    if (state & Button3Mask) {
        mod |= PP_INPUTEVENT_MODIFIER_RIGHTBUTTONDOWN;
    }

    return mod;
}

struct call_plugin_handle_input_event_param_s {
    struct pp_instance_s       *pp_i;
    PP_Resource                 event_id;
};

static
void
_call_ppp_handle_input_event_comt(void *user_data, int32_t result)
{
    struct call_plugin_handle_input_event_param_s *p = user_data;

    if (p->pp_i->ppp_input_event)
        p->pp_i->ppp_input_event->HandleInputEvent(p->pp_i->id, p->event_id);

    ppb_core_release_resource(p->event_id);
    g_slice_free(struct call_plugin_handle_input_event_param_s, p);
}

static
void
ppp_handle_input_event_helper(struct pp_instance_s *pp_i, PP_Resource event_id)
{
    struct call_plugin_handle_input_event_param_s *p = g_slice_alloc0(sizeof(*p));
    p->pp_i = pp_i;
    p->event_id = event_id;
    ppb_core_call_on_main_thread(0, PP_MakeCCB(_call_ppp_handle_input_event_comt, p), PP_OK);
}

static
int16_t
handle_enter_leave_event(NPP npp, void *event)
{
    XCrossingEvent *ev = event;
    struct pp_instance_s *pp_i = npp->pdata;

    // quit if plugin doesn't handle input events
    if (!pp_i->ppp_input_event)
        return 0;

    const uint32_t combined_mask = pp_i->event_mask | pp_i->filtered_event_mask;
    if (!(PP_INPUTEVENT_CLASS_MOUSE & combined_mask))
        return 0;

    struct PP_Point mouse_position = {.x = ev->x, .y = ev->y};
    struct PP_Point zero_point = {.x = 0, .y = 0};
    unsigned int mod = x_state_mask_to_pp_inputevent_modifier(ev->state);
    PP_InputEvent_Type event_type = (ev->type == EnterNotify) ? PP_INPUTEVENT_TYPE_MOUSEENTER
                                                              : PP_INPUTEVENT_TYPE_MOUSELEAVE;
    PP_Resource pp_event;
    pp_event = ppb_mouse_input_event_create(pp_i->id, event_type,
                                            ev->time/1.0e6, mod, PP_INPUTEVENT_MOUSEBUTTON_NONE,
                                            &mouse_position, 0, &zero_point);
    ppp_handle_input_event_helper(pp_i, pp_event);

    return 1;
}

static
int16_t
handle_motion_event(NPP npp, void *event)
{
    XMotionEvent *ev = event;
    struct pp_instance_s *pp_i = npp->pdata;

    // quit if plugin doesn't handle input events
    if (!pp_i->ppp_input_event)
        return 0;

    const uint32_t combined_mask = pp_i->event_mask | pp_i->filtered_event_mask;
    if (!(PP_INPUTEVENT_CLASS_MOUSE & combined_mask))
        return 0;

    struct PP_Point mouse_position = {.x = ev->x, .y = ev->y};
    struct PP_Point zero_point = {.x = 0, .y = 0};
    unsigned int mod = x_state_mask_to_pp_inputevent_modifier(ev->state);
    PP_Resource pp_event;

    pp_event = ppb_mouse_input_event_create(pp_i->id, PP_INPUTEVENT_TYPE_MOUSEMOVE,
                                            ev->time/1.0e6, mod, PP_INPUTEVENT_MOUSEBUTTON_NONE,
                                            &mouse_position, 0, &zero_point);
    ppp_handle_input_event_helper(pp_i, pp_event);
    return 1;
}

static
int16_t
handle_button_press_release_event(NPP npp, void *event)
{
    XButtonEvent *ev = event;
    struct pp_instance_s *pp_i = npp->pdata;
    uint32_t event_class = 0;
    PP_InputEvent_MouseButton mouse_button = PP_INPUTEVENT_MOUSEBUTTON_NONE;

    // quit if plugin doesn't handle input events
    if (!pp_i->ppp_input_event)
        return 0;

    struct PP_Point mouse_position = {.x = ev->x, .y = ev->y};
    struct PP_Point zero_point = {.x = 0, .y = 0};
    unsigned int mod = x_state_mask_to_pp_inputevent_modifier(ev->state);
    float wheel_x = 0.0, wheel_y = 0.0;
    int ev_button = ev->button;

    if (config.quirks.switch_buttons_2_3) {
        if (ev_button == 2)
            ev_button = 3;
        else if (ev_button == 3)
            ev_button = 2;
    }

    switch (ev_button) {
    case 1:
        mouse_button = PP_INPUTEVENT_MOUSEBUTTON_LEFT;
        event_class = PP_INPUTEVENT_CLASS_MOUSE;
        break;
    case 2:
        mouse_button = PP_INPUTEVENT_MOUSEBUTTON_MIDDLE;
        event_class = PP_INPUTEVENT_CLASS_MOUSE;
        break;
    case 3:
        mouse_button = PP_INPUTEVENT_MOUSEBUTTON_RIGHT;
        event_class = PP_INPUTEVENT_CLASS_MOUSE;
        break;

    case 4: // wheel up
        wheel_y = -1;
        event_class = PP_INPUTEVENT_CLASS_WHEEL;
        break;
    case 5: // wheel down
        wheel_y = 1;
        event_class = PP_INPUTEVENT_CLASS_WHEEL;
        break;
    case 6: // wheel left
        wheel_x = -1;
        event_class = PP_INPUTEVENT_CLASS_WHEEL;
        break;
    case 7: // wheel right
        wheel_x = 1;
        event_class = PP_INPUTEVENT_CLASS_WHEEL;
        break;
    }

    // TODO: Firefox does not pass wheel event to windowless plugins.
    //       Only windowed plugins can receive such events.


    const uint32_t combined_mask = pp_i->event_mask | pp_i->filtered_event_mask;
    if (!(event_class & combined_mask))
        return 0;

    if (event_class == PP_INPUTEVENT_CLASS_MOUSE) {
        PP_Resource         pp_event;
        PP_InputEvent_Type  event_type;

        event_type = (ev->type == ButtonPress) ? PP_INPUTEVENT_TYPE_MOUSEDOWN
                                               : PP_INPUTEVENT_TYPE_MOUSEUP;
        pp_event = ppb_mouse_input_event_create(pp_i->id, event_type,
                                                ev->time/1.0e6, mod, mouse_button,
                                                &mouse_position, 1, &zero_point);
        ppp_handle_input_event_helper(pp_i, pp_event);

        // context menu event
        if (ev->type == ButtonRelease && ev_button == 3) {
            pp_event = ppb_mouse_input_event_create(pp_i->id,
                                                    PP_INPUTEVENT_TYPE_CONTEXTMENU,
                                                    ev->time/1.0e6, mod, mouse_button,
                                                    &mouse_position, 1, &zero_point);
            ppp_handle_input_event_helper(pp_i, pp_event);
        }
    } else { // event_class == PP_INPUTEVENT_CLASS_WHEEL
        const float scroll_by_tick = 10.0;
        struct PP_FloatPoint wheel_delta = { .x = wheel_x * scroll_by_tick,
                                             .y = wheel_y * scroll_by_tick };
        struct PP_FloatPoint wheel_ticks = { .x = wheel_x, .y = wheel_y };

        // pp_event = ppb_wheel_input_event_create(
        //                 pp_i->id, ev->time/1.0e6, mod,
        //                 &wheel_delta, &wheel_ticks, PP_FALSE);
        (void)wheel_delta;
        (void)wheel_ticks;
        return 0;
    }

    return 1;
}

int
is_printable_sequence(const char *s, size_t len)
{
    const unsigned char *u = (const unsigned char *)s;
    if (len == 1 && 0x20 <= u[0] && u[0] <= 0x7e)
        return 1;
    if (len > 1)
        return 1;
    return 0;
}

int16_t
handle_key_press_release_event(NPP npp, void *event)
{
    XKeyEvent            *ev = event;
    struct pp_instance_s *pp_i = npp->pdata;
    PP_InputEvent_Type    event_type;

    // quit if plugin doesn't handle input events
    if (!pp_i->ppp_input_event)
        return 0;

    const uint32_t combined_mask = pp_i->event_mask | pp_i->filtered_event_mask;
    if (!(PP_INPUTEVENT_CLASS_KEYBOARD & combined_mask))
        return 0;

    char           buffer[20];
    KeySym         keysym;
    XComposeStatus compose_status;
    int            charcount;
    int            pp_keycode;
    PP_Resource    pp_event;
    unsigned int   mod;

    charcount = XLookupString(ev, buffer, sizeof(buffer), &keysym, &compose_status);
    pp_keycode = xkeycode_to_pp_keycode(keysym);
    mod = x_state_mask_to_pp_inputevent_modifier(ev->state);
    mod = mod | get_left_right_pp_flag(keysym);

    // left flag is always set, it needs to be dropped if there is right flag
    if (mod & PP_INPUTEVENT_MODIFIER_ISRIGHT)
        mod = mod & (~(unsigned)PP_INPUTEVENT_MODIFIER_ISLEFT);

    event_type = (ev->type == KeyPress) ? PP_INPUTEVENT_TYPE_KEYDOWN
                                        : PP_INPUTEVENT_TYPE_KEYUP;

    if (ev->type == KeyPress && is_printable_sequence(buffer, charcount)) {
        struct PP_Var character_text = ppb_var_var_from_utf8(buffer, charcount);
        pp_event = ppb_keyboard_input_event_create_1_0(
                        pp_i->id, PP_INPUTEVENT_TYPE_CHAR, ev->time/1.0e6, mod,
                        pp_keycode, character_text);
        ppb_var_release(character_text);

        ppp_handle_input_event_helper(pp_i, pp_event);
    }

    pp_event = ppb_keyboard_input_event_create_1_0(pp_i->id, event_type, ev->time/1.0e6,
                                                   mod, pp_keycode, PP_MakeUndefined());
    ppp_handle_input_event_helper(pp_i, pp_event);
    return 1;
}

static
void
_call_ppp_did_change_focus_comt(void *user_data, int32_t result)
{
    struct pp_instance_s *pp_i = user_data;
    PP_Bool has_focus = result;

    // determine whenever we should pass focus event to the plugin instance
    pthread_mutex_lock(&display.lock);
    int muffle_event = (pp_i->ignore_focus_events_cnt > 0);
    if (pp_i->ignore_focus_events_cnt > 0)
        pp_i->ignore_focus_events_cnt -= 1;
    pthread_mutex_unlock(&display.lock);

    if (pp_i->ppp_instance_1_1 && pp_i->ppp_instance_1_1->DidChangeFocus && !muffle_event)
        pp_i->ppp_instance_1_1->DidChangeFocus(pp_i->id, has_focus);
}

int16_t
handle_focus_in_out_event(NPP npp, void *event)
{
    struct pp_instance_s *pp_i = npp->pdata;
    XFocusChangeEvent *ev = event;

    PP_Bool has_focus = (ev->type == FocusIn) ? PP_TRUE : PP_FALSE;

    struct PP_CompletionCallback ccb;
    ccb = PP_MakeCCB(_call_ppp_did_change_focus_comt, pp_i);
    ppb_core_call_on_main_thread(0, ccb, has_focus);
    return 1;
}

int16_t
NPP_HandleEvent(NPP npp, void *event)
{
    XAnyEvent *xaev = event;
    struct pp_instance_s *pp_i = npp->pdata;

    if (config.quirks.plugin_missing) {
        if (xaev->type == GraphicsExpose)
            handle_placeholder_graphics_expose_event(npp, event);
        return NPERR_NO_ERROR;
    }

    if (pp_i && pp_i->is_fullscreen && pp_i->fs_wnd != xaev->window) {
        return 0;
    }

#define TRACE_HELPER(implstatus, f)                                                     \
    trace_info_##f("[NPP] " implstatus " %s npp=%p, event={.type=%s, .serial=%lu, "     \
               ".send_event=%d, .display=%p, .window=0x%x}\n", __func__, npp,           \
               reverse_xevent_type(xaev->type), xaev->serial, xaev->send_event,         \
               xaev->display, (uint32_t)xaev->window)

    switch (xaev->type) {
    case Expose:
        TRACE_HELPER("{full}", f);
        // its event have similar layout to GraphicsExpose, so let ge handler to do the work
        return handle_graphics_expose_event(npp, event);
    case GraphicsExpose:
        TRACE_HELPER("{full}", f);
        return handle_graphics_expose_event(npp, event);
    case EnterNotify:
        TRACE_HELPER("{full}", f);
        return handle_enter_leave_event(npp, event);
    case LeaveNotify:
        TRACE_HELPER("{full}", f);
        return handle_enter_leave_event(npp, event);
    case MotionNotify:
        TRACE_HELPER("{full}", f);
        return handle_motion_event(npp, event);
    case ButtonPress:
        TRACE_HELPER("{full}", f);
        return handle_button_press_release_event(npp, event);
    case ButtonRelease:
        TRACE_HELPER("{full}", f);
        return handle_button_press_release_event(npp, event);
    case KeyPress:
        TRACE_HELPER("{full}", f);
        return handle_key_press_release_event(npp, event);
    case KeyRelease:
        TRACE_HELPER("{full}", f);
        return handle_key_press_release_event(npp, event);
    case FocusIn:
        TRACE_HELPER("{full}", f);
        return handle_focus_in_out_event(npp, event);
    case FocusOut:
        TRACE_HELPER("{full}", f);
        return handle_focus_in_out_event(npp, event);
    default:
        TRACE_HELPER("{zilch}", z);
        return 0;
    }
#undef TRACE_HELPER
}

void
NPP_URLNotify(NPP npp, const char *url, NPReason reason, void *notifyData)
{
    trace_info_f("[NPP] {full} %s npp=%p, url=%s, reason=%d, notifyData=%u\n", __func__,
                 npp, url, reason, (unsigned)(size_t)notifyData);
    // This is no-op. We are handling request in NPP_NewStream function.
    return;
}

NPError
NPP_GetValue(NPP npp, NPPVariable variable, void *value)
{
    struct pp_instance_s *pp_i = npp->pdata;
    if (config.quirks.plugin_missing)
        return NPERR_NO_ERROR;

    const char *var_name = reverse_npp_variable(variable);

    switch (variable) {
    case NPPVpluginNameString:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginDescriptionString:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginWindowBool:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginTransparentBool:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVjavaClass:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginWindowSize:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginTimerInterval:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginScriptableInstance:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginScriptableIID:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVjavascriptPushCallerBool:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginKeepLibraryInMemory:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginNeedsXEmbed:
        trace_info_f("[NPP] {full} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        *(int *)value = 0;
        break;
    case NPPVpluginScriptableNPObject:
        trace_info_f("[NPP] {full} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        *(void **)value = pp_i->scriptable_obj;
        break;
    case NPPVformValue:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginUrlRequestsDisplayedBool:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginWantsAllNetworkStreams:
        trace_info_f("[NPP] {full} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        *(int *)value = 1;
        break;
    case NPPVpluginNativeAccessibleAtkPlugId:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginCancelSrcStream:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVsupportsAdvancedKeyHandling:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginUsesDOMForCursorBool:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    case NPPVpluginDrawingModel:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    default:
        trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s\n", __func__, npp, var_name);
        break;
    }

    return NPERR_NO_ERROR;
}

NPError
NPP_SetValue(NPP npp, NPNVariable variable, void *value)
{
    trace_info_z("[NPP] {zilch} %s npp=%p, variable=%s, value=%p\n", __func__,
                 npp, reverse_npn_variable(variable), value);
    return NPERR_NO_ERROR;
}

NPBool
NPP_GotFocus(NPP npp, NPFocusDirection direction)
{
    trace_info_z("[NPP] {zilch} %s npp=%p, direction=%d\n", __func__, npp, direction);
    return 1;
}

void
NPP_LostFocus(NPP npp)
{
    trace_info_z("[NPP] {zilch} %s npp=%p\n", __func__, npp);
    return;
}

void
NPP_URLRedirectNotify(NPP npp, const char *url, int32_t status, void *notifyData)
{
    trace_info_f("[NPP] {full} %s npp=%p, url=%s, status=%d, notifyData=%u\n", __func__,
                 npp, url, status, (unsigned)(size_t)notifyData);

    PP_Resource loader = (size_t)notifyData;
    if (loader) {
        struct pp_url_loader_s *ul = pp_resource_acquire(loader, PP_RESOURCE_URL_LOADER);
        if (ul) {
            free_and_nullify(ul->redirect_url);
            ul->redirect_url = strdup(url);
            pp_resource_release(loader);
        }
    }

    // We are handling redirects ourselves. Tell browser to stop.
    npn.urlredirectresponse(npp, notifyData, false);
    return;
}

NPError
NPP_ClearSiteData(const char *site, uint64_t flags, uint64_t maxAge)
{
    trace_info_z("[NPP] {zilch} %s site=%s, flags=%"PRIu64", maxAge=%"PRIu64"\n", __func__,
                 site, flags, maxAge);
    return NPERR_NO_ERROR;
}

char**
NPP_GetSitesWithData(void)
{
    trace_info_z("[NPP] {zilch} %s\n", __func__);
    return NULL;
}

void
NPP_DidComposite(NPP npp)
{
    trace_info_z("[NPP] {zilch} %s npp=%p\n", __func__, npp);
    return;
}
