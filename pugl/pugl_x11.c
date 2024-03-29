/*
  Copyright 2012-2015 David Robillard <http://drobilla.net>
  Copyright 2013 Robin Gareus <robin@gareus.org>
  Copyright 2011-2012 Ben Loftis, Harrison Consoles

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/**
   @file pugl_x11.c X11 Pugl Implementation.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#ifdef PUGL_HAVE_GL
#include <GL/gl.h>
#include <GL/glx.h>
#endif

#ifdef PUGL_HAVE_CAIRO
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#endif

#include "pugl/event.h"
#include "pugl/pugl_internal.h"

struct PuglInternalsImpl {
	Display*         display;
	int              screen;
	Window           win;
	XIM              xim;
	XIC              xic;
#ifdef PUGL_HAVE_CAIRO
	cairo_surface_t* surface;
	cairo_t*         cr;
#endif
#ifdef PUGL_HAVE_GL
	GLXContext       ctx;
	Bool             doubleBuffered;
#endif
    int dnd_x;
    int dnd_y;
};

PuglInternals*
puglInitInternals()
{
	return (PuglInternals*)calloc(1, sizeof(PuglInternals));
}

static XVisualInfo*
getVisual(PuglView* view)
{
	PuglInternals* const impl = view->impl;
	XVisualInfo*         vi   = NULL;

#ifdef PUGL_HAVE_GL
	if (view->ctx_type == PUGL_GL) {
		// Try to create double-buffered visual
		int double_attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER,
		                       GLX_RED_SIZE,   4,
		                       GLX_GREEN_SIZE, 4,
		                       GLX_BLUE_SIZE,  4,
                               GLX_ALPHA_SIZE, 4,
		                       GLX_DEPTH_SIZE, 24,
                               GLX_STENCIL_SIZE, 8,
		                       None };
		vi = glXChooseVisual(impl->display, impl->screen, double_attrs);
		if (!vi) {
			// Failed, create single-buffered visual
			int single_attrs[] = { GLX_RGBA,
			                       GLX_RED_SIZE,   4,
			                       GLX_GREEN_SIZE, 4,
			                       GLX_BLUE_SIZE,  4,
                                   GLX_ALPHA_SIZE, 4,
			                       GLX_DEPTH_SIZE, 24,
                                   GLX_STENCIL_SIZE, 8,
			                       None };
			vi = glXChooseVisual(impl->display, impl->screen, single_attrs);
			impl->doubleBuffered = False;
		} else {
			impl->doubleBuffered = True;
		}
	}
#endif
#ifdef PUGL_HAVE_CAIRO
	if (view->ctx_type == PUGL_CAIRO) {
		XVisualInfo pat;
		int         n;
		pat.screen = impl->screen;
		vi         = XGetVisualInfo(impl->display, VisualScreenMask, &pat, &n);
	}
#endif

	return vi;
}

static void
createContext(PuglView* view, XVisualInfo* vi)
{
	PuglInternals* const impl = view->impl;

#ifdef PUGL_HAVE_GL
	if (view->ctx_type == PUGL_GL) {
		impl->ctx = glXCreateContext(impl->display, vi, 0, GL_TRUE);
	}
#endif
#ifdef PUGL_HAVE_CAIRO
	if (view->ctx_type == PUGL_CAIRO) {
		view->impl->surface = cairo_xlib_surface_create(
			impl->display, impl->win, vi->visual, view->width, view->height);
		if (!(impl->cr = cairo_create(view->impl->surface))) {
			fprintf(stderr, "failed to create cairo context\n");
		}
	}
#endif
}

static void
destroyContext(PuglView* view)
{
#ifdef PUGL_HAVE_GL
	if (view->ctx_type == PUGL_GL) {
		glXDestroyContext(view->impl->display, view->impl->ctx);
	}
#endif
#ifdef PUGL_HAVE_CAIRO
	if (view->ctx_type == PUGL_CAIRO) {
		cairo_destroy(view->impl->cr);
		cairo_surface_destroy(view->impl->surface);
	}
#endif
}

void
puglEnterContext(PuglView* view)
{
#ifdef PUGL_HAVE_GL
	if (view->ctx_type == PUGL_GL) {
		glXMakeCurrent(view->impl->display, view->impl->win, view->impl->ctx);
	}
#endif
}

void
puglLeaveContext(PuglView* view, bool flush)
{
#ifdef PUGL_HAVE_GL
	if (view->ctx_type == PUGL_GL && flush) {
		glFlush();
		if (view->impl->doubleBuffered) {
			glXSwapBuffers(view->impl->display, view->impl->win);
		}
	}
#endif
}

int
puglCreateWindow(PuglView* view, const char* title)
{
	PuglInternals* const impl = view->impl;

	impl->display = XOpenDisplay(0);
	impl->screen  = DefaultScreen(impl->display);

	XVisualInfo* const vi = getVisual(view);
	if (!vi) {
		return 1;
	}

	Window xParent = view->parent
		? (Window)view->parent
		: RootWindow(impl->display, impl->screen);

	Colormap cmap = XCreateColormap(
		impl->display, xParent, vi->visual, AllocNone);

	XSetWindowAttributes attr;
	memset(&attr, 0, sizeof(XSetWindowAttributes));
	attr.background_pixel = BlackPixel(impl->display, impl->screen);
	attr.border_pixel     = BlackPixel(impl->display, impl->screen);
	attr.colormap         = cmap;
	attr.event_mask       = (ExposureMask | StructureNotifyMask |
	                         EnterWindowMask | LeaveWindowMask |
	                         KeyPressMask | KeyReleaseMask |
	                         ButtonPressMask | ButtonReleaseMask |
	                         PointerMotionMask | FocusChangeMask);

	impl->win = XCreateWindow(
		impl->display, xParent,
		0, 0, view->width, view->height, 0, vi->depth, InputOutput, vi->visual,
		CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attr);

	createContext(view, vi);

	XSizeHints sizeHints;
	memset(&sizeHints, 0, sizeof(sizeHints));
	if (!view->resizable) {
		sizeHints.flags      = PMinSize|PMaxSize;
		sizeHints.min_width  = view->width;
		sizeHints.min_height = view->height;
		sizeHints.max_width  = view->width;
		sizeHints.max_height = view->height;
		XSetNormalHints(impl->display, impl->win, &sizeHints);
	} else {
		if (view->min_width || view->min_height) {
			sizeHints.flags      = PMinSize;
			sizeHints.min_width  = view->min_width;
			sizeHints.min_height = view->min_height;
		}
		if (view->min_aspect_x) {
			sizeHints.flags        |= PAspect;
			sizeHints.min_aspect.x  = view->min_aspect_x;
			sizeHints.min_aspect.y  = view->min_aspect_y;
			sizeHints.max_aspect.x  = view->max_aspect_x;
			sizeHints.max_aspect.y  = view->max_aspect_y;
		}

		XSetNormalHints(impl->display, impl->win, &sizeHints);
	}

	if (title) {
		XStoreName(impl->display, impl->win, title);
	}

	if (!view->parent) {
		Atom wmDelete = XInternAtom(impl->display, "WM_DELETE_WINDOW", True);
		XSetWMProtocols(impl->display, impl->win, &wmDelete, 1);
	}

	if (view->transient_parent) {
		XSetTransientForHint(impl->display, impl->win,
		                     (Window)(view->transient_parent));
	}

	if (!(impl->xim = XOpenIM(impl->display, NULL, NULL, NULL))) {
		XSetLocaleModifiers("@im=");
		if (!(impl->xim = XOpenIM(impl->display, NULL, NULL, NULL))) {
			fprintf(stderr, "warning: XOpenIM failed\n");
		}
	}

	if (!(impl->xic = XCreateIC(impl->xim, XNInputStyle,
	                            XIMPreeditNothing | XIMStatusNothing,
	                            XNClientWindow, impl->win,
	                            XNFocusWindow, impl->win,
	                            NULL))) {
		fprintf(stderr, "warning: XCreateIC failed\n");
	}

	XFree(vi);

    unsigned char version = 4;
    Atom dnd_aware = XInternAtom(impl->display, "XdndAware", false);
    XChangeProperty(impl->display, impl->win, dnd_aware, XA_ATOM,
            32, PropModeReplace, &version, 1);

	return 0;
}

void
puglShowWindow(PuglView* view)
{
	XMapRaised(view->impl->display, view->impl->win);
}

void
puglHideWindow(PuglView* view)
{
	XUnmapWindow(view->impl->display, view->impl->win);
}

void
puglDestroy(PuglView* view)
{
	if (!view) {
		return;
	}

	destroyContext(view);
	XDestroyWindow(view->impl->display, view->impl->win);
	XCloseDisplay(view->impl->display);
	free(view->windowClass);
	free(view->impl);
	free(view);
	view = NULL;
}

static PuglKey
keySymToSpecial(KeySym sym)
{
	switch (sym) {
	case XK_F1:        return PUGL_KEY_F1;
	case XK_F2:        return PUGL_KEY_F2;
	case XK_F3:        return PUGL_KEY_F3;
	case XK_F4:        return PUGL_KEY_F4;
	case XK_F5:        return PUGL_KEY_F5;
	case XK_F6:        return PUGL_KEY_F6;
	case XK_F7:        return PUGL_KEY_F7;
	case XK_F8:        return PUGL_KEY_F8;
	case XK_F9:        return PUGL_KEY_F9;
	case XK_F10:       return PUGL_KEY_F10;
	case XK_F11:       return PUGL_KEY_F11;
	case XK_F12:       return PUGL_KEY_F12;
	case XK_Left:      return PUGL_KEY_LEFT;
	case XK_Up:        return PUGL_KEY_UP;
	case XK_Right:     return PUGL_KEY_RIGHT;
	case XK_Down:      return PUGL_KEY_DOWN;
	case XK_Page_Up:   return PUGL_KEY_PAGE_UP;
	case XK_Page_Down: return PUGL_KEY_PAGE_DOWN;
	case XK_Home:      return PUGL_KEY_HOME;
	case XK_End:       return PUGL_KEY_END;
	case XK_Insert:    return PUGL_KEY_INSERT;
	case XK_Shift_L:   return PUGL_KEY_SHIFT;
	case XK_Shift_R:   return PUGL_KEY_SHIFT;
	case XK_Control_L: return PUGL_KEY_CTRL;
	case XK_Control_R: return PUGL_KEY_CTRL;
	case XK_Alt_L:     return PUGL_KEY_ALT;
	case XK_Alt_R:     return PUGL_KEY_ALT;
	case XK_Super_L:   return PUGL_KEY_SUPER;
	case XK_Super_R:   return PUGL_KEY_SUPER;
	}
	return (PuglKey)0;
}

static void
translateKey(PuglView* view, XEvent* xevent, PuglEvent* event)
{
	KeySym sym = 0;
	char*  str = (char*)event->key.utf8;
	memset(str, 0, 8);
	event->key.filter = XFilterEvent(xevent, None);
	if (xevent->type == KeyRelease || event->key.filter || !view->impl->xic) {
		if (XLookupString(&xevent->xkey, str, 7, &sym, NULL) == 1) {
			event->key.character = str[0];
			//ASCII for control characters
			if(sym == (sym&0x7f)) {
				event->key.character = sym;
				event->key.utf8[0] = sym;
			}
		}
	} else {
		/* TODO: Not sure about this.  On my system, some characters work with
		   Xutf8LookupString but not with XmbLookupString, and some are the
		   opposite. */
		Status status = 0;
#ifdef X_HAVE_UTF8_STRING
		const int n = Xutf8LookupString(
			view->impl->xic, &xevent->xkey, str, 7, &sym, &status);
#else
		const int n = XmbLookupString(
			view->impl->xic, &xevent->xkey, str, 7, &sym, &status);
#endif
		if (n > 0) {
			event->key.character = puglDecodeUTF8((const uint8_t*)str);
			//ASCII for control characters
			if(sym == (sym&0x7f)) {
				event->key.character = sym;
				event->key.utf8[0] = sym;
			}
		}

	}
	event->key.special = keySymToSpecial(sym);
	event->key.keycode = xevent->xkey.keycode;
}

static unsigned
translateModifiers(unsigned xstate)
{
	unsigned state = 0;
	state |= (xstate & ShiftMask)   ? PUGL_MOD_SHIFT  : 0;
	state |= (xstate & ControlMask) ? PUGL_MOD_CTRL   : 0;
	state |= (xstate & Mod1Mask)    ? PUGL_MOD_ALT    : 0;
	state |= (xstate & Mod4Mask)    ? PUGL_MOD_SUPER  : 0;
	return state;
}

static PuglEvent
translateEvent(PuglView* view, XEvent xevent)
{
	PuglEvent event;
	memset(&event, 0, sizeof(event));

	event.any.view       = view;
	event.any.send_event = xevent.xany.send_event;

	switch (xevent.type) {
	case ConfigureNotify:
		event.type             = PUGL_CONFIGURE;
		event.configure.x      = xevent.xconfigure.x;
		event.configure.y      = xevent.xconfigure.y;
		event.configure.width  = xevent.xconfigure.width;
		event.configure.height = xevent.xconfigure.height;
		break;
	case Expose:
		event.type          = PUGL_EXPOSE;
		event.expose.x      = xevent.xexpose.x;
		event.expose.y      = xevent.xexpose.y;
		event.expose.width  = xevent.xexpose.width;
		event.expose.height = xevent.xexpose.height;
		event.expose.count  = xevent.xexpose.count;
		break;
	case MotionNotify:
		event.type           = PUGL_MOTION_NOTIFY;
		event.motion.time    = xevent.xmotion.time;
		event.motion.x       = xevent.xmotion.x;
		event.motion.y       = xevent.xmotion.y;
		event.motion.x_root  = xevent.xmotion.x_root;
		event.motion.y_root  = xevent.xmotion.y_root;
		event.motion.state   = translateModifiers(xevent.xmotion.state);
		event.motion.is_hint = (xevent.xmotion.is_hint == NotifyHint);
		break;
	case ButtonPress:
		if (xevent.xbutton.button >= 4 && xevent.xbutton.button <= 7) {
			event.type           = PUGL_SCROLL;
			event.scroll.time    = xevent.xbutton.time;
			event.scroll.x       = xevent.xbutton.x;
			event.scroll.y       = xevent.xbutton.y;
			event.scroll.x_root  = xevent.xbutton.x_root;
			event.scroll.y_root  = xevent.xbutton.y_root;
			event.scroll.state   = translateModifiers(xevent.xbutton.state);
			event.scroll.dx      = 0.0;
			event.scroll.dy      = 0.0;
			switch (xevent.xbutton.button) {
			case 4: event.scroll.dy =  1.0f; break;
			case 5: event.scroll.dy = -1.0f; break;
			case 6: event.scroll.dx = -1.0f; break;
			case 7: event.scroll.dx =  1.0f; break;
			}
		}
		// nobreak
	case ButtonRelease:
		if (xevent.xbutton.button < 4 || xevent.xbutton.button > 7) {
			event.button.type   = ((xevent.type == ButtonPress)
			                       ? PUGL_BUTTON_PRESS
			                       : PUGL_BUTTON_RELEASE);
			event.button.time   = xevent.xbutton.time;
			event.button.x      = xevent.xbutton.x;
			event.button.y      = xevent.xbutton.y;
			event.button.x_root = xevent.xbutton.x_root;
			event.button.y_root = xevent.xbutton.y_root;
			event.button.state  = translateModifiers(xevent.xbutton.state);
			event.button.button = xevent.xbutton.button;
		}
		break;
	case KeyPress:
	case KeyRelease:
		event.type       = ((xevent.type == KeyPress)
		                    ? PUGL_KEY_PRESS
		                    : PUGL_KEY_RELEASE);
		event.key.time   = xevent.xkey.time;
		event.key.x      = xevent.xkey.x;
		event.key.y      = xevent.xkey.y;
		event.key.x_root = xevent.xkey.x_root;
		event.key.y_root = xevent.xkey.y_root;
		event.key.state  = translateModifiers(xevent.xkey.state);
		translateKey(view, &xevent, &event);
		break;
	case EnterNotify:
	case LeaveNotify:
		event.type            = ((xevent.type == EnterNotify)
		                         ? PUGL_ENTER_NOTIFY
		                         : PUGL_LEAVE_NOTIFY);
		event.crossing.time   = xevent.xcrossing.time;
		event.crossing.x      = xevent.xcrossing.x;
		event.crossing.y      = xevent.xcrossing.y;
		event.crossing.x_root = xevent.xcrossing.x_root;
		event.crossing.y_root = xevent.xcrossing.y_root;
		event.crossing.state  = translateModifiers(xevent.xcrossing.state);
		event.crossing.mode   = PUGL_CROSSING_NORMAL;
		if (xevent.xcrossing.mode == NotifyGrab) {
			event.crossing.mode = PUGL_CROSSING_GRAB;
		} else if (xevent.xcrossing.mode == NotifyUngrab) {
			event.crossing.mode = PUGL_CROSSING_UNGRAB;
		}
		break;

	case FocusIn:
	case FocusOut:
		event.type = ((xevent.type == EnterNotify)
		              ? PUGL_FOCUS_IN
		              : PUGL_FOCUS_OUT);
		event.focus.grab = (xevent.xfocus.mode != NotifyNormal);
		break;

	default:
		break;
	}

	return event;
}

void
puglGrabFocus(PuglView* view)
{
	XSetInputFocus(
		view->impl->display, view->impl->win, RevertToPointerRoot, CurrentTime);
}

PuglStatus
puglWaitForEvent(PuglView* view)
{
	XEvent xevent;
	XPeekEvent(view->impl->display, &xevent);
	return PUGL_SUCCESS;
}

PuglStatus
puglProcessEvents(PuglView* view)
{
	XEvent xevent;
	while (XPending(view->impl->display) > 0) {
		XNextEvent(view->impl->display, &xevent);
		if (xevent.type == ClientMessage) {
			// Handle close message
			char* type = XGetAtomName(view->impl->display,
			                          xevent.xclient.message_type);
			if (!strcmp(type, "WM_PROTOCOLS") && view->closeFunc) {
				view->closeFunc(view);
				view->redisplay = false;
			} else if(!strcmp(type, "XdndPosition")) {
                int global_x = (xevent.xclient.data.l[2]>>16) & 0xFFFF;
                int global_y = (xevent.xclient.data.l[2]>>00) & 0xFFFF;
                int win_x = 0, win_y = 0;
                int local_x = 0, local_y = 0;
                Window child;
                XTranslateCoordinates(view->impl->display, view->impl->win,
                        RootWindow(view->impl->display, view->impl->screen), 0, 0, &win_x, &win_y, &child);
                view->impl->dnd_x = global_x - win_x;
                view->impl->dnd_y = global_y - win_y;

                //printf("xdnd at (%d, %d) <%d %d %d %d>\n", local_x, local_y, global_x, global_y, win_x, win_y);
                view->motionFunc(view, view->impl->dnd_x, view->impl->dnd_y);


                Window ev_source = xevent.xclient.data.l[0];
                XEvent reply;
                memset(&reply, 0, sizeof(reply));
                reply.xany.type = ClientMessage;
                reply.xany.display = view->impl->display;
                reply.xclient.window = ev_source;
                reply.xclient.format = 32;
                reply.xclient.message_type = XInternAtom(view->impl->display, "XdndStatus", false);
                reply.xclient.data.l[0] = xevent.xclient.window;
                reply.xclient.data.l[1] = 1;
                reply.xclient.data.l[2] = 0;
                reply.xclient.data.l[3] = 0;
                reply.xclient.data.l[4] = XInternAtom(view->impl->display, "XdndActionPrivate", false);
                XSendEvent(view->impl->display, ev_source, false, NoEventMask, &reply);
                XFlush(view->impl->display);

                //printf("X drag and drop position...\n");
            } else if(!strcmp(type, "XdndEnter")) {
                //printf("X drag and drop enter...\n");
            } else if(!strcmp(type, "XdndLeave")) {
                //printf("X drag and drop leave...\n");
            } else if(!strcmp(type, "XdndDrop")) {
                //printf("X drag and drop drop...\n");
                view->mouseFunc(view, 4, 1, view->impl->dnd_x, view->impl->dnd_y);
                //printf("mousefunc called...\n");
            }

			XFree(type);
			continue;
		} else if (xevent.type == KeyRelease) {
			// Ignore key repeat if necessary
			if (view->ignoreKeyRepeat &&
			    XEventsQueued(view->impl->display, QueuedAfterReading)) {
				XEvent next;
				XPeekEvent(view->impl->display, &next);
				if (next.type == KeyPress &&
				    next.xkey.time == xevent.xkey.time &&
				    next.xkey.keycode == xevent.xkey.keycode) {
					XNextEvent(view->impl->display, &xevent);
					continue;
				}
			}
		} else if (xevent.type == FocusIn) {
			XSetICFocus(view->impl->xic);
		} else if (xevent.type == FocusOut) {
			XUnsetICFocus(view->impl->xic);
		}

		// Translate X11 event to Pugl event
		const PuglEvent event = translateEvent(view, xevent);

#ifdef PUGL_HAVE_CAIRO
		if (event.type == PUGL_CONFIGURE && view->ctx_type == PUGL_CAIRO) {
			// Resize surfaces/contexts before dispatching
			cairo_xlib_surface_set_size(view->impl->surface,
			                            event.configure.width,
			                            event.configure.height);
			view->redisplay = true;
		}
#endif

		// Dispatch event to application
		if (!(view->redisplay && event.type == PUGL_EXPOSE)) {
			puglDispatchEvent(view, &event);
		}
	}

	if (view->redisplay) {
		const PuglEventExpose expose = {
			PUGL_EXPOSE, view, true, 0, 0, view->width, view->height, 0
		};
		puglDispatchEvent(view, (const PuglEvent*)&expose);
	}

	return PUGL_SUCCESS;
}

void
puglPostRedisplay(PuglView* view)
{
	view->redisplay = true;
}

PuglNativeWindow
puglGetNativeWindow(PuglView* view)
{
	return view->impl->win;
}

void*
puglGetContext(PuglView* view)
{
#ifdef PUGL_HAVE_CAIRO
	if (view->ctx_type == PUGL_CAIRO) {
		return view->impl->cr;
	}
#endif
	return NULL;
}
