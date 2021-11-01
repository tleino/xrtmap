/*
 * xrtmap - real time point plotting on a map for X11
 * Copyright (c) 2021 Tommi Leino <namhas@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <png.h>

#define EXPIRE_SECS 30

static Display *dpy;
static Window win;
static int width, height;
static int win_width, win_height;
static XImage *image;
static GC gc;
static int ctlfd;
static double scale_x, scale_y;

struct point {
	double lat;
	double lon;
	time_t t;
	struct point *next;
};

struct point *first;
struct point *last;

static void
enqueue_point(double lat, double lon)
{
	struct point *pt;

	pt = calloc(1, sizeof(struct point));
	if (pt == NULL)
		err(1, "calloc point");
	pt->lat = lat;
	pt->lon = lon;
	pt->t = time(NULL);
	if (last == NULL)
		first = last = pt;
	else {
		last->next = pt;
		last = pt;
	}
}

static void
dequeue_point()
{
	struct point *pt;

	if (first == NULL)
		return;

	pt = first;
	first = pt->next;
	if (first == NULL)
		last = NULL;
	free(pt);
}

static void
open_dpy()
{
	char *denv;

	denv = getenv("DISPLAY");
	if (denv == NULL && errno != 0)
		err(1, "getenv DISPLAY");
	dpy = XOpenDisplay(denv);
	if (dpy == NULL) {
		if (denv == NULL)
			errx(1, "X11 connection failed; "
			    "DISPLAY environment variable not set?");
               	else
                       	errx(1, "failed X11 connection to '%s'", denv);
	}
}

static void
create_window()
{
	int x, y, w, h;
	XSetWindowAttributes a;
	unsigned long v;

	w = 360*2;
	h = 180*2;
	x = 0;
	y = 0;
	v = CWBackPixel;
	a.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
	win = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y, w, h, 0,
	    CopyFromParent, InputOutput, CopyFromParent, v, &a);
	XStoreName(dpy, win, "xrtmap");
	XMapWindow(dpy, win);
	XRaiseWindow(dpy, win);

	win_width = w;
	win_height = h;
}

static void
load_image(const char *path)
{
	FILE *fp;
	char header[8];
	int x, y;
	png_structp pngp;
	png_infop infop;
	unsigned char *xdata;
	png_bytep *rowp;
	int depth;
	int bytes_per_pixel;
	int i;
	unsigned long val;

	fp = fopen(path, "r");
	if (fp == NULL)
		err(1, "%s", path);

	fread(header, 1, 8, fp);
	if (png_sig_cmp(header, 0, 8))
		errx(1, "%s: not a PNG file", path);

	pngp = png_create_read_struct(
	    PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!pngp)
		errx(1, "png_create_read_struct failed");
	infop = png_create_info_struct(pngp);
	if (!infop)
		errx(1, "png_create_info_struct failed");
	if (setjmp(png_jmpbuf(pngp)))
		errx(1, "error during png_jmpbuf");
	png_init_io(pngp, fp);
	png_set_sig_bytes(pngp, 8);
	png_read_info(pngp, infop);

	width = png_get_image_width(pngp, infop);
	height = png_get_image_height(pngp, infop);
	depth = png_get_bit_depth(pngp, infop);

	if (setjmp(png_jmpbuf(pngp)))
		errx(1, "error setting png_jmpbuf");
	rowp = (png_bytep*) calloc(1, sizeof(png_bytep) * height);
	if (rowp == NULL)
		err(1, "malloc rowp");
	for (y = 0; y < height; y++) {
		rowp[y] = (png_byte*) calloc(1, png_get_rowbytes(pngp, infop));
		if (rowp[y] == NULL)
			errx(1, "malloc rowp[%d]", y);
	}

	bytes_per_pixel = png_get_rowbytes(pngp, infop) / width;

	png_read_image(pngp, rowp);

	depth = DisplayPlanes(dpy, DefaultScreen(dpy));
	xdata = malloc(4 * width * height);
	if (xdata == NULL)
		err(1, "malloc xdata");

	image = XCreateImage(dpy,
	    CopyFromParent,
	    depth,
	    XYPixmap, 0, (char *) xdata, width, height, 32, 0);
	if (image == NULL)
		errx(1, "XCreateImage failed");	

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			val = 0;
			for (i = 0; i < bytes_per_pixel; i++) {
				val |= (rowp[y][x * bytes_per_pixel + i] 
				    << (8 * (bytes_per_pixel - i - 1)));
			}
			XPutPixel(image, x, y, val);
		}
	}

	fclose(fp);
}

static void
draw_point(double lat, double lon)
{
	int x, y;
	int sz;

	sz = 5;
	x = (int) ((lon + 180.0) * scale_x);
	y = (int) ((90.0 - lat) * scale_y);
	XFillRectangle(dpy, win, gc, x-(sz/2), y-(sz/2), sz, sz);
}

static void
undraw_point(double lat, double lon)
{
	int x, y;
	int sz;

	sz = 5;
	x = (int) ((lon + 180.0) * scale_x);
	y = (int) ((90.0 - lat) * scale_y);
	XPutImage(dpy, win, gc, image, x-(sz/2), y-(sz/2), x-(sz/2), y-(sz/2),
	    sz, sz);
}

static void
draw()
{
	struct point *np;

	XPutImage(dpy, win, gc, image, 0, 0, 0, 0, width, height);

	for (np = first; np != NULL; np = np->next)
		draw_point(np->lat, np->lon);
}

static void
plot(double lat, double lon)
{
	enqueue_point(lat, lon);

	draw_point(lat, lon);
	XFlush(dpy);
}

static void
process_input(int fd)
{
	char buf[256];
	ssize_t n;
	double lat, lon;

	n = read(fd, buf, sizeof(buf));
	if (n <= 0)
		err(1, "read");
	buf[n] = '\0';

	if (sscanf(buf, "%lf %lf", &lat, &lon) == 2)
		plot(lat, lon);
	else
		warnx("format error: use <lat> <lon>");
}

static void
process_xevents()
{
	XEvent event;

	while (XPending(dpy)) {
		XNextEvent(dpy, &event);
		switch (event.type) {
		case Expose:
			if (event.xexpose.count == 0)
				draw();
			break;
		case ConfigureNotify:
			win_width = event.xconfigure.width;
			win_height = event.xconfigure.height;
			break;
		}
	}
	XFlush(dpy);
}

static void
process_events()
{
	fd_set rfds;
	int nready, i;
	int maxfd;
	int xfd;
	struct timeval tv, *tvp;
	time_t t;

	xfd = ConnectionNumber(dpy);
	process_xevents();

	FD_ZERO(&rfds);
	FD_SET(xfd, &rfds);
	maxfd = xfd;
	FD_SET(ctlfd, &rfds);
	if (ctlfd > maxfd)
		maxfd = ctlfd;

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	/*
	 * We add timeout only if we have points that we have to clear
	 * away.
	 */
	if (first != NULL)
		tvp = &tv;
	else
		tvp = NULL;

	nready = select(maxfd + 1, &rfds, NULL, NULL, tvp);
	if (nready == -1)
		err(1, "select");

	for (i = 0; i < nready; i++)
		if (FD_ISSET(xfd, &rfds))
			process_xevents();
		else if (FD_ISSET(ctlfd, &rfds))
			process_input(ctlfd);

	t = time(NULL);
	while (first != NULL && t - first->t >= EXPIRE_SECS) {
		undraw_point(first->lat, first->lon);
		dequeue_point();
	}
}

int
main(int argc, char **argv)
{
	int running;
	char *path;
	XGCValues v;
	XColor def, exact;

	open_dpy();
	create_window();

	if (XAllocNamedColor(dpy, XDefaultColormap(dpy, DefaultScreen(dpy)),
	    "red", &def, &exact) == 0)
		errx(1, "couldn't allocate 'red'");

	v.foreground = def.pixel;
	gc = XCreateGC(dpy, win, GCForeground, &v);

	if (argc < 2) {
		fprintf(stderr, "usage: %s PNG_IMAGE\n", *argv);
		return 1;
	}
	path = *++argv;

	load_image(path);

	scale_x = width/360.0;
	scale_y = height/180.0;

	draw();

	XSync(dpy, False);
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);

	ctlfd = STDIN_FILENO;
	running = 1;
	while (running)
		process_events();

	XSync(dpy, False);
	XCloseDisplay(dpy);
	return 0;
}
