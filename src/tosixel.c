/*
 * this file is derived from "sixel" original version (2014-3-2)
 * http://nanno.dip.jp/softlib/man/rlogin/sixel.tar.gz
 *
 * Initial developer of this file is kmiya@culti.
 *
 * He distributes it under very permissive license which permits
 * useing, copying, modification, redistribution, and all other
 * public activities without any restrictions.
 *
 * He declares this is compatible with MIT/BSD/GPL.
 *
 * Hayaki Saito (user@zuse.jp) modified this and re-licensed
 * it under the MIT license.
 *
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif

#include "output.h"
#include "dither.h"
#include "sixel.h"

/* implementation */

static void
penetrate(sixel_output_t *context, int nwrite)
{
    int pos;
    for (pos = 0; pos < nwrite; pos += 508) {
        context->fn_write("\x1bP", 2, context->priv);
        context->fn_write(((char *)context->buffer)+pos,
                nwrite - pos < 508 ? nwrite - pos : 508, context->priv);
        context->fn_write("\x1b\\", 2, context->priv);
    }
}


static void
sixel_advance(sixel_output_t *context, int nwrite)
{
    if ((context->pos += nwrite) >= SIXEL_OUTPUT_PACKET_SIZE) {
        if (context->penetrate_multiplexer) {
            penetrate(context, SIXEL_OUTPUT_PACKET_SIZE);
        } else {
            context->fn_write((char *)context->buffer, SIXEL_OUTPUT_PACKET_SIZE, context->priv);
        }
        memcpy(context->buffer,
               context->buffer + SIXEL_OUTPUT_PACKET_SIZE,
               (context->pos -= SIXEL_OUTPUT_PACKET_SIZE));
    }
}


static int
sixel_put_flash(sixel_output_t *const context)
{
    int n;
    int nwrite;

#if defined(USE_VT240)        /* VT240 Max 255 ? */
    while (context->save_count > 255) {
        nwrite = spritf((char *)context->buffer + context->pos, "!255%c", context->save_pixel);
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
        context->save_count -= 255;
    }
#endif  /* defined(USE_VT240) */

    if (context->save_count > 3) {
        /* DECGRI Graphics Repeat Introducer ! Pn Ch */
        nwrite = sprintf((char *)context->buffer + context->pos, "!%d%c", context->save_count, context->save_pixel);
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
    } else {
        for (n = 0; n < context->save_count; n++) {
            context->buffer[context->pos] = (char)context->save_pixel;
            sixel_advance(context, 1);
        }
    }

    context->save_pixel = 0;
    context->save_count = 0;

    return 0;
}


static void
sixel_put_pixel(sixel_output_t *const context, int pix)
{
    if (pix < 0 || pix > '?') {
        pix = 0;
    }

    pix += '?';

    if (pix == context->save_pixel) {
        context->save_count++;
    } else {
        sixel_put_flash(context);
        context->save_pixel = pix;
        context->save_count = 1;
    }
}


static void
sixel_node_del(sixel_output_t *const context, sixel_node_t *np)
{
    sixel_node_t *tp;

    if ((tp = context->node_top) == np) {
        context->node_top = np->next;
    }

    else {
        while (tp->next != NULL) {
            if (tp->next == np) {
                tp->next = np->next;
                break;
            }
            tp = tp->next;
        }
    }

    np->next = context->node_free;
    context->node_free = np;
}


static int
sixel_put_node(sixel_output_t *const context, int x,
        sixel_node_t *np, int ncolors, int keycolor)
{
    int nwrite;

    if (ncolors != 2 || keycolor == -1) {
        /* designate palette index */
        if (context->active_palette != np->pal) {
            nwrite = sprintf((char *)context->buffer + context->pos,
                             "#%d", context->conv_palette[np->pal]);
            sixel_advance(context, nwrite);
            context->active_palette = np->pal;
        }
    }

    for (; x < np->sx; x++) {
        if (x != keycolor) {
            sixel_put_pixel(context, 0);
        }
    }

    for (; x < np->mx; x++) {
        if (x != keycolor) {
            sixel_put_pixel(context, np->map[x]);
        }
    }

    sixel_put_flash(context);

    return x;
}


static int
sixel_encode_impl(unsigned char *pixels, int width, int height,
                  unsigned char *palette, int ncolors, int keycolor,
                  int bodyonly, sixel_output_t *context)
{
    int x, y, i, n, c;
    int sx, mx;
    int len, pix;
    unsigned char *map;
    sixel_node_t *np, *tp, top;
    unsigned char list[SIXEL_PALETTE_MAX];
    int nwrite;
    int p[3] = {0, 0, 0};
    int pcount = 3;
    int use_raster_attributes = 1;

    context->pos = 0;

    if (ncolors < 1) {
        return (-1);
    }
    len = ncolors * width;
    context->active_palette = (-1);

#if HAVE_CALLOC
    if ((map = (unsigned char *)calloc(len, sizeof(unsigned char))) == NULL) {
        return (-1);
    }
#else
    if ((map = (unsigned char *)malloc(len)) == NULL) {
        return (-1);
    }
    memset(map, 0, len);
#endif
    for (n = 0; n < ncolors; n++) {
        context->conv_palette[n] = list[n] = n;
    }

    if (!context->skip_dcs_envelope) {
        if (context->has_8bit_control) {
            nwrite = sprintf((char *)context->buffer, "\x90");
        } else {
            nwrite = sprintf((char *)context->buffer, "\x1bP");
        }
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
    }

    if (p[2] == 0) {
        pcount--;
        if (p[1] == 0) {
            pcount--;
            if (p[0] == 0) {
                pcount--;
            }
        }
    }

    if (pcount > 0) {
        nwrite = sprintf((char *)context->buffer + context->pos, "%d", p[0]);
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
        if (pcount > 1) {
            nwrite = sprintf((char *)context->buffer + context->pos, ";%d", p[1]);
            if (nwrite <= 0) {
                return (-1);
            }
            sixel_advance(context, nwrite);
            if (pcount > 2) {
                nwrite = sprintf((char *)context->buffer + context->pos, ";%d", p[2]);
                if (nwrite <= 0) {
                    return (-1);
                }
                sixel_advance(context, nwrite);
            }
        }
    }

    nwrite = sprintf((char *)context->buffer + context->pos, "q");
    if (nwrite <= 0) {
        return (-1);
    }
    sixel_advance(context, nwrite);

    if (use_raster_attributes) {
        nwrite = sprintf((char *)context->buffer + context->pos, "\"1;1;%d;%d", width, height);
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
    }

    if (!bodyonly && (ncolors != 2 || keycolor == -1)) {
        if (context->palette_type == PALETTETYPE_HLS) {
            for (n = 0; n < ncolors; n++) {
                int h;
                int l;
                int s;
                int r, g, b, max, min;
                r = palette[n * 3 + 0];
                g = palette[n * 3 + 1];
                b = palette[n * 3 + 2];
                max = r > g ? (r > b ? r: b): (g > b ? g: b);
                min = r < g ? (r < b ? r: b): (g < b ? g: b);
                l = ((max + min) * 100 + 255) / 510;
                if (max == min) {
                    h = s = 0;
                } else {
                    if (l < 50) {
                        s = ((max - min) * 100 + 127) / (max + min);
                    } else {
                        s = ((max - min) * 100 + 127) / ((255 - max) + (255 - min));
                    }
                    if (r == max) {
                        h = 120 + (g - b) * 60 / (max - min);
                    } else if (g == max) {
                        h = 240 + (b - r) * 60 / (max - min);
                    } else if (r < g) /* if (b == max) */ {
                        h = 360 + (r - g) * 60 / (max - min);
                    } else {
                        h = 0 + (r - g) * 60 / (max - min);
                    }
                }
                /* DECGCI Graphics Color Introducer  # Pc ; Pu; Px; Py; Pz */
                nwrite = sprintf((char *)context->buffer + context->pos, "#%d;1;%d;%d;%d",
                                 context->conv_palette[n], h, l, s);
                if (nwrite <= 0) {
                    return (-1);
                }
                sixel_advance(context, nwrite);
                if (nwrite <= 0) {
                    return (-1);
                }
            }
        } else {
            for (n = 0; n < ncolors; n++) {
                /* DECGCI Graphics Color Introducer  # Pc ; Pu; Px; Py; Pz */
                nwrite = sprintf((char *)context->buffer + context->pos, "#%d;2;%d;%d;%d",
                                 context->conv_palette[n],
                                 (palette[n * 3 + 0] * 100 + 127) / 255,
                                 (palette[n * 3 + 1] * 100 + 127) / 255,
                                 (palette[n * 3 + 2] * 100 + 127) / 255);
                if (nwrite <= 0) {
                    return (-1);
                }
                sixel_advance(context, nwrite);
                if (nwrite <= 0) {
                    return (-1);
                }
            }
        }
    }

    for (y = i = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            pix = pixels[y * width + x];
            if (pix >= 0 && pix < ncolors && pix != keycolor) {
                map[pix * width + x] |= (1 << i);
            }
        }

        if (++i < 6 && (y + 1) < height) {
            continue;
        }

        for (c = 0; c < ncolors; c++) {
            for (sx = 0; sx < width; sx++) {
                if (*(map + c * width + sx) == 0) {
                    continue;
                }

                for (mx = sx + 1; mx < width; mx++) {
                    if (*(map + c * width + mx) != 0) {
                        continue;
                    }

                    for (n = 1; (mx + n) < width; n++) {
                        if (*(map + c * width + mx + n) != 0) {
                            break;
                        }
                    }

                    if (n >= 10 || (mx + n) >= width) {
                        break;
                    }
                    mx = mx + n - 1;
                }

                if ((np = context->node_free) != NULL) {
                    context->node_free = np->next;
                } else if ((np = (sixel_node_t *)malloc(sizeof(sixel_node_t))) == NULL) {
                    return (-1);
                }

                np->pal = c;
                np->sx = sx;
                np->mx = mx;
                np->map = map + c * width;

                top.next = context->node_top;
                tp = &top;

                while (tp->next != NULL) {
                    if (np->sx < tp->next->sx) {
                        break;
                    } else if (np->sx == tp->next->sx && np->mx > tp->next->mx) {
                        break;
                    }
                    tp = tp->next;
                }

                np->next = tp->next;
                tp->next = np;
                context->node_top = top.next;

                sx = mx - 1;
            }

        }

        if (y != 5) {
            /* DECGNL Graphics Next Line */
            context->buffer[context->pos] = '-';
            sixel_advance(context, 1);
            if (nwrite <= 0) {
                return (-1);
            }
        }

        for (x = 0; (np = context->node_top) != NULL;) {
            if (x > np->sx) {
                /* DECGCR Graphics Carriage Return */
                context->buffer[context->pos] = '$';
                sixel_advance(context, 1);
                x = 0;
            }

            x = sixel_put_node(context, x, np, ncolors, keycolor);
            sixel_node_del(context, np);
            np = context->node_top;

            while (np != NULL) {
                if (np->sx < x) {
                    np = np->next;
                    continue;
                }

                x = sixel_put_node(context, x, np, ncolors, keycolor);
                sixel_node_del(context, np);
                np = context->node_top;
            }
        }

        i = 0;
        memset(map, 0, len);
    }

    if (!context->skip_dcs_envelope && !context->penetrate_multiplexer) {
        if (context->has_8bit_control) {
            nwrite = sprintf((char *)context->buffer + context->pos, "\x9c");
        } else {
            nwrite = sprintf((char *)context->buffer + context->pos, "\x1b\\");
        }
        if (nwrite <= 0) {
            return (-1);
        }
        sixel_advance(context, nwrite);
    }

    /* flush buffer */
    if (context->pos > 0) {
        if (context->penetrate_multiplexer) {
            penetrate(context, context->pos);
            context->fn_write("\x1bP\x1b\x1b\\\x1bP\\\x1b\\", 10, context->priv);
        }
        else {
            context->fn_write((char *)context->buffer, context->pos, context->priv);
        }
    }

    /* free nodes */
    while ((np = context->node_free) != NULL) {
        context->node_free = np->next;
        free(np);
    }

    free(map);

    return 0;
}

int sixel_encode(unsigned char  /* in */ *pixels,   /* pixel bytes */
                 int            /* in */ width,     /* image width */
                 int            /* in */ height,    /* image height */
                 int            /* in */ depth,     /* pixel depth */
                 sixel_dither_t /* in */ *dither,   /* dither context */
                 sixel_output_t /* in */ *context)  /* output context */
{
    unsigned char *paletted_pixels;

    sixel_dither_ref(dither);

    /* apply palette */
    paletted_pixels = sixel_apply_palette(pixels, width, height, dither);
    if (paletted_pixels == NULL) {
        sixel_dither_unref(dither);
        return (-1);
    }

    sixel_encode_impl(paletted_pixels, width, height,
                      dither->palette, dither->ncolors,
                      dither->keycolor, dither->bodyonly, context);

    sixel_dither_unref(dither);
    free(paletted_pixels);

    return 0;
}


/* emacs, -*- Mode: C; tab-width: 4; indent-tabs-mode: nil -*- */
/* vim: set expandtab ts=4 : */
/* EOF */
