/*
 *
 * mediancut algorithm implementation is imported from pnmcolormap.c
 * in netpbm library.
 * http://netpbm.sourceforge.net/ 
 *
 * *******************************************************************************
 *                  original license block of pnmcolormap.c
 * *******************************************************************************
 * 
 *   Derived from ppmquant, originally by Jef Poskanzer.
 * 
 *   Copyright (C) 1989, 1991 by Jef Poskanzer.
 *   Copyright (C) 2001 by Bryan Henderson.
 * 
 *   Permission to use, copy, modify, and distribute this software and its
 *   documentation for any purpose and without fee is hereby granted, provided
 *   that the above copyright notice appear in all copies and that both that
 *   copyright notice and this permission notice appear in supporting
 *   documentation.  This software is provided "as is" without express or
 *   implied warranty.
 * 
 * ******************************************************************************
 *
 * Copyright (c) 2014 Hayaki Saito
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */

#include "config.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>

#if defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#endif

#if 0
#define quant_trace fprintf
#else
static inline void quant_trace(FILE *f, ...) {}
#endif

/* exported function */
extern uint8_t *
make_palette(uint8_t *data, int x, int y, int depth, int reqcolors, int *ncolors);

extern uint8_t *
apply_palette(unsigned char *data, int x, int y, int depth, uint8_t *palette, int ncolors);

/*****************************************************************************
 *
 * quantization
 *
 *****************************************************************************/

typedef struct box* boxVector;
struct box {
    int ind;
    int colors;
    int sum;
};

typedef unsigned long sample;
typedef sample * tuple;
enum methodForRep {REP_CENTER_BOX, REP_AVERAGE_COLORS, REP_AVERAGE_PIXELS};
 
#pragma pack(1)
struct tupleint {
    /* An ordered pair of a tuple value and an integer, such as you 
       would find in a tuple table or tuple hash.
       Note that this is a variable length structure.
    */
    int value;
    sample tuple[1];  
    /* This is actually a variable size array -- its size is the 
       depth of the tuple in question.  Some compilers do not let us
       declare a variable length array.
    */
};
#pragma pack(0)
typedef struct tupleint ** tupletable;

#pragma pack(1)
typedef struct {
    unsigned int size;
    tupletable table;
} tupletable2;
#pragma pack(0)

static unsigned int compareplanePlane;
    /* This is a parameter to compareplane().  We use this global variable
       so that compareplane() can be called by qsort(), to compare two
       tuples.  qsort() doesn't pass any arguments except the two tuples.
    */
static int
compareplane(const void * const arg1, 
             const void * const arg2) {

    const struct tupleint * const * const comparandPP  = arg1;
    const struct tupleint * const * const comparatorPP = arg2;

    return (*comparandPP)->tuple[compareplanePlane] -
        (*comparatorPP)->tuple[compareplanePlane];
}


static int
sumcompare(const void * const b1, const void * const b2)
{
    return(((boxVector)b2)->sum - ((boxVector)b1)->sum);
}


static tupletable const
alloctupletable(unsigned int const depth, unsigned int const size)
{
    if (UINT_MAX / sizeof(struct tupleint) < size) {
        quant_trace(stderr, "size %u is too big for arithmetic\n", size);
        return NULL;
    }

    unsigned int const mainTableSize = size * sizeof(struct tupleint *);
    unsigned int const tupleIntSize = 
        sizeof(struct tupleint) - sizeof(sample) 
        + depth * sizeof(sample);

    /* To save the enormous amount of time it could take to allocate
       each individual tuple, we do a trick here and allocate everything
       as a single malloc block and suballocate internally.
    */
    if ((UINT_MAX - mainTableSize) / tupleIntSize < size) {
        quant_trace(stderr, "size %u is too big for arithmetic\n", size);
        return NULL;
    }

    unsigned int const allocSize = mainTableSize + size * tupleIntSize;
    void * pool;
    
    pool = malloc(allocSize);

    if (!pool) {
        quant_trace(stderr, "Unable to allocate %u bytes for a %u-entry "
                  "tuple table\n", allocSize, size);
        return NULL;
    }
    tupletable const tbl = (tupletable) pool;

    unsigned int i;

    for (i = 0; i < size; ++i)
        tbl[i] = (struct tupleint *)
            ((char*)pool + mainTableSize + i * tupleIntSize);

    return tbl;
}



/*
** Here is the fun part, the median-cut colormap generator.  This is based
** on Paul Heckbert's paper "Color Image Quantization for Frame Buffer
** Display", SIGGRAPH '82 Proceedings, page 297.
*/

static tupletable2
newColorMap(unsigned int const newcolors, unsigned int const depth)
{

    tupletable2 colormap;
    unsigned int i;
    tupletable table;

    colormap.size = 0;
    colormap.table = alloctupletable(depth, newcolors);
    if (colormap.table) {
        for (i = 0; i < newcolors; ++i) {
            unsigned int plane;
            for (plane = 0; plane < depth; ++plane) 
                colormap.table[i]->tuple[plane] = 0;
        }
        colormap.size = newcolors;
    }

    return colormap;
}


static boxVector
newBoxVector(int const colors, int const sum, int const newcolors)
{
    boxVector bv;
    bv = (boxVector)malloc(sizeof(struct box) * newcolors);
    if (bv == NULL)
        quant_trace(stderr, "out of memory allocating box vector table\n");

    /* Set up the initial box. */
    bv[0].ind = 0;
    bv[0].colors = colors;
    bv[0].sum = sum;

    return bv;
}


static void
findBoxBoundaries(tupletable2  const colorfreqtable,
                  unsigned int const depth,
                  unsigned int const boxStart,
                  unsigned int const boxSize,
                  sample             minval[],
                  sample             maxval[]) {
/*----------------------------------------------------------------------------
  Go through the box finding the minimum and maximum of each
  component - the boundaries of the box.
-----------------------------------------------------------------------------*/
    unsigned int plane;
    unsigned int i;

    for (plane = 0; plane < depth; ++plane) {
        minval[plane] = colorfreqtable.table[boxStart]->tuple[plane];
        maxval[plane] = minval[plane];
    }

    for (i = 1; i < boxSize; ++i) {
        unsigned int plane;
        for (plane = 0; plane < depth; ++plane) {
            sample const v = colorfreqtable.table[boxStart + i]->tuple[plane];
            if (v < minval[plane]) minval[plane] = v;
            if (v > maxval[plane]) maxval[plane] = v;
        } 
    }
}



static unsigned int
largestByNorm(sample minval[], sample maxval[], unsigned int const depth) {
    
    unsigned int largestDimension;
    unsigned int plane;

    sample largestSpreadSoFar = 0;  
    largestDimension = 0;
    for (plane = 0; plane < depth; ++plane) {
        sample const spread = maxval[plane]-minval[plane];
        if (spread > largestSpreadSoFar) {
            largestDimension = plane;
            largestSpreadSoFar = spread;
        }
    }
    return largestDimension;
}




static void
centerBox(int          const boxStart,
          int          const boxSize,
          tupletable2  const colorfreqtable, 
          unsigned int const depth,
          tuple        const newTuple) {

    unsigned int plane;

    for (plane = 0; plane < depth; ++plane) {
        int minval, maxval;
        unsigned int i;

        minval = maxval = colorfreqtable.table[boxStart]->tuple[plane];
        
        for (i = 1; i < boxSize; ++i) {
            int const v = colorfreqtable.table[boxStart + i]->tuple[plane];
            minval = minval < v ? minval: v;
            maxval = maxval > v ? maxval: v;
        }
        newTuple[plane] = (minval + maxval) / 2;
    }
}



static void
averageColors(int          const boxStart,
              int          const boxSize,
              tupletable2  const colorfreqtable, 
              unsigned int const depth,
              tuple        const newTuple) {

    unsigned int plane;

    for (plane = 0; plane < depth; ++plane) {
        sample sum;
        int i;

        sum = 0;

        for (i = 0; i < boxSize; ++i) 
            sum += colorfreqtable.table[boxStart+i]->tuple[plane];

        newTuple[plane] = sum / boxSize;
    }
}



static void
averagePixels(int          const boxStart,
              int          const boxSize,
              tupletable2  const colorfreqtable, 
              unsigned int const depth,
              tuple        const newTuple) {

    unsigned int n;
        /* Number of tuples represented by the box */
    unsigned int plane;
    unsigned int i;

    /* Count the tuples in question */
    n = 0;  /* initial value */
    for (i = 0; i < boxSize; ++i)
        n += colorfreqtable.table[boxStart + i]->value;


    for (plane = 0; plane < depth; ++plane) {
        sample sum;
        int i;

        sum = 0;

        for (i = 0; i < boxSize; ++i) 
            sum += colorfreqtable.table[boxStart+i]->tuple[plane]
                * colorfreqtable.table[boxStart+i]->value;

        newTuple[plane] = sum / n;
    }
}



static tupletable2
colormapFromBv(unsigned int      const newcolors,
               boxVector         const bv, 
               unsigned int      const boxes,
               tupletable2       const colorfreqtable, 
               unsigned int      const depth,
               enum methodForRep const methodForRep) {
    /*
    ** Ok, we've got enough boxes.  Now choose a representative color for
    ** each box.  There are a number of possible ways to make this choice.
    ** One would be to choose the center of the box; this ignores any structure
    ** within the boxes.  Another method would be to average all the colors in
    ** the box - this is the method specified in Heckbert's paper.  A third
    ** method is to average all the pixels in the box. 
    */
    tupletable2 colormap;
    unsigned int bi;

    colormap = newColorMap(newcolors, depth);
    if (!colormap.size) {
        return colormap;
    }

    for (bi = 0; bi < boxes; ++bi) {
        switch (methodForRep) {
        case REP_CENTER_BOX: 
            centerBox(bv[bi].ind, bv[bi].colors, colorfreqtable, depth, 
                      colormap.table[bi]->tuple);
            break;
        case REP_AVERAGE_COLORS:
            averageColors(bv[bi].ind, bv[bi].colors, colorfreqtable, depth,
                          colormap.table[bi]->tuple);
            break;
        case REP_AVERAGE_PIXELS:
            averagePixels(bv[bi].ind, bv[bi].colors, colorfreqtable, depth,
                          colormap.table[bi]->tuple);
            break;
        default:
            quant_trace(stderr, "Internal error: invalid value of methodForRep: %d\n",
                        methodForRep);
        }
    }
    return colormap;
}


static void
splitBox(boxVector             const bv, 
         unsigned int *        const boxesP, 
         unsigned int          const bi,
         tupletable2           const colorfreqtable, 
         unsigned int          const depth)
{
/*----------------------------------------------------------------------------
   Split Box 'bi' in the box vector bv (so that bv contains one more box
   than it did as input).  Split it so that each new box represents about
   half of the pixels in the distribution given by 'colorfreqtable' for 
   the colors in the original box, but with distinct colors in each of the
   two new boxes.

   Assume the box contains at least two colors.
-----------------------------------------------------------------------------*/
    unsigned int const boxStart = bv[bi].ind;
    unsigned int const boxSize  = bv[bi].colors;
    unsigned int const sm       = bv[bi].sum;

    sample * minval;  /* malloc'ed array */
    sample * maxval;  /* malloc'ed array */

    unsigned int largestDimension;
        /* number of the plane with the largest spread */
    unsigned int medianIndex;
    int lowersum;
        /* Number of pixels whose value is "less than" the median */

    minval = (sample *)malloc(depth);
    maxval = (sample *)malloc(depth);

    findBoxBoundaries(colorfreqtable, depth, boxStart, boxSize, 
                      minval, maxval);

    /* Find the largest dimension, and sort by that component.  I have
       included two methods for determining the "largest" dimension;
       first by simply comparing the range in RGB space, and second by
       transforming into luminosities before the comparison.
    */
    largestDimension = largestByNorm(minval, maxval, depth);
    free(minval);
    free(maxval);
                                                    
    /* TODO: I think this sort should go after creating a box,
       not before splitting.  Because you need the sort to use
       the REP_CENTER_BOX method of choosing a color to
       represent the final boxes 
    */

    /* Set the gross global variable 'compareplanePlane' as a
       parameter to compareplane(), which is called by qsort().
    */
    compareplanePlane = largestDimension;
    qsort((char*) &colorfreqtable.table[boxStart], boxSize, 
          sizeof(colorfreqtable.table[boxStart]), 
          compareplane);
            
    {
        /* Now find the median based on the counts, so that about half
           the pixels (not colors, pixels) are in each subdivision.  */

        unsigned int i;

        lowersum = colorfreqtable.table[boxStart]->value; /* initial value */
        for (i = 1; i < boxSize - 1 && lowersum < sm/2; ++i) {
            lowersum += colorfreqtable.table[boxStart + i]->value;
        }
        medianIndex = i;
    }
    /* Split the box, and sort to bring the biggest boxes to the top.  */

    bv[bi].colors = medianIndex;
    bv[bi].sum = lowersum;
    bv[*boxesP].ind = boxStart + medianIndex;
    bv[*boxesP].colors = boxSize - medianIndex;
    bv[*boxesP].sum = sm - lowersum;
    ++(*boxesP);
    qsort((char*) bv, *boxesP, sizeof(struct box), sumcompare);
}



static void
mediancut(tupletable2           const colorfreqtable, 
          unsigned int          const depth,
          int                   const newcolors,
          enum methodForRep     const methodForRep,
          tupletable2 *         const colormapP) {
/*----------------------------------------------------------------------------
   Compute a set of only 'newcolors' colors that best represent an
   image whose pixels are summarized by the histogram
   'colorfreqtable'.  Each tuple in that table has depth 'depth'.
   colorfreqtable.table[i] tells the number of pixels in the subject image
   have a particular color.

   As a side effect, sort 'colorfreqtable'.
-----------------------------------------------------------------------------*/
    boxVector bv;
    unsigned int bi;
    unsigned int boxes;
    int multicolorBoxesExist;
    unsigned int i;
    unsigned int sum;

    sum = 0;

    for (i = 0; i < colorfreqtable.size; ++i)
        sum += colorfreqtable.table[i]->value;

        /* There is at least one box that contains at least 2 colors; ergo,
           there is more splitting we can do.
        */

    bv = newBoxVector(colorfreqtable.size, sum, newcolors);
    boxes = 1;
    multicolorBoxesExist = (colorfreqtable.size > 1);

    /* Main loop: split boxes until we have enough. */
    while (boxes < newcolors && multicolorBoxesExist) {
        /* Find the first splittable box. */
        for (bi = 0; bi < boxes && bv[bi].colors < 2; ++bi);
        if (bi >= boxes)
            multicolorBoxesExist = 0;
        else 
            splitBox(bv, &boxes, bi, colorfreqtable, depth);
    }
    *colormapP = colormapFromBv(newcolors, bv, boxes,
                                colorfreqtable, depth,
                                methodForRep);

    free(bv);
}


static void
computeHistogram(unsigned char *data,
                 unsigned int length,
                 unsigned long const depth,
                 tupletable2 * const colorfreqtableP)
{
    unsigned int i, n;
    unsigned char *histgram;
    unsigned short *refmap;
    unsigned short *ref;
    struct tupleint *t;
    unsigned int index;
    unsigned int step;
    const unsigned int max_sample = 4096;

    quant_trace(stderr, "making histogram...\n");

    histgram = (unsigned char *)malloc((1 << depth * 5) * sizeof(*histgram));
    memset(histgram, 0, 1 << depth * 5 * sizeof(*histgram));
    ref = refmap = (unsigned short *)malloc(max_sample * sizeof(*refmap));

    colorfreqtableP->size = 0;
    colorfreqtableP->table = malloc(sizeof(void *) * (1 << 15));

    if (length > max_sample * depth) {
        step = length / depth / max_sample;
    } else {
        step = depth;
    }

    for (i = 0; i < length; i += step) {
        index = 0;
        for (n = 0; n < depth; n ++) {
            index |= data[i + depth - 1 - n] >> 3 << n * 5;
        }
        if (histgram[index] == 0) { 
            *ref++ = index;
        }
        if (histgram[index] < 255) { 
            histgram[index]++;
        }
    }

    colorfreqtableP->table = alloctupletable(depth, (ref - refmap) / sizeof(ref));
    while (ref-- != refmap) {
        if (histgram[*ref] > 0) {
            t = (struct tupleint *)malloc(sizeof(int) + sizeof(sample) * depth);
            t->value = histgram[*ref];
            for (n = 0; n < depth; n++) {
                t->tuple[depth - 1 - n] = (*ref >> n * 5 & 0x1f) << 3;
            }
            colorfreqtableP->table[colorfreqtableP->size] = t;
            colorfreqtableP->size++;
        }
    }

    free(refmap);
    free(histgram);

    quant_trace(stderr, "%u colors found\n", colorfreqtableP->size);
}


static void
computeColorMapFromInput(unsigned char *data,
                         size_t length,
                         unsigned int const depth, 
                         int const reqColors, 
                         enum methodForRep const methodForRep,
                         tupletable2 * const colormapP)
{
/*----------------------------------------------------------------------------
   Produce a colormap containing the best colors to represent the
   image stream in file 'ifP'.  Figure it out using the median cut
   technique.

   The colormap will have 'reqcolors' or fewer colors in it, unless
   'allcolors' is true, in which case it will have all the colors that
   are in the input.

   The colormap has the same maxval as the input.

   Put the colormap in newly allocated storage as a tupletable2 
   and return its address as *colormapP.  Return the number of colors in
   it as *colorsP and its maxval as *colormapMaxvalP.

   Return the characteristics of the input file as
   *formatP and *freqPamP.  (This information is not really
   relevant to our colormap mission; just a fringe benefit).
-----------------------------------------------------------------------------*/
    tupletable2 colorfreqtable;

    computeHistogram(data, length, depth, &colorfreqtable);
    
    if (colorfreqtable.size <= reqColors) {
        quant_trace(stderr, "Image already has few enough colors (<=%d).  "
                   "Keeping same colors.\n", reqColors);
        *colormapP = colorfreqtable;
    } else {
        quant_trace(stderr, "choosing %d colors...\n", reqColors);
        mediancut(colorfreqtable, depth, reqColors, methodForRep, colormapP);
        free(colorfreqtable.table);
    }
}


unsigned char *
make_palette(unsigned char *data, int x, int y, int depth, int reqcolors, int *ncolors)
{
    int i;
    unsigned char *palette;
    tupletable2 colormap;

    computeColorMapFromInput(data, x * y * depth, depth,
                             reqcolors, REP_CENTER_BOX, &colormap);
    *ncolors = colormap.size;
    quant_trace(stderr, "tupletable size: %d", *ncolors);
    palette = malloc(*ncolors * depth);
    for (i = 0; i < *ncolors; i++) {
        palette[i * depth + 0] = colormap.table[i]->tuple[0];
        palette[i * depth + 1] = colormap.table[i]->tuple[1];
        palette[i * depth + 2] = colormap.table[i]->tuple[2];
        quant_trace(stderr, "(%lu, %lu, %lu) -> %d\n",
                    colormap.table[i]->tuple[0],
                    colormap.table[i]->tuple[1],
                    colormap.table[i]->tuple[2],
                    colormap.table[i]->value);
    }
    free(colormap.table);
    return palette;
}

void add_offset(unsigned char *data, int i, int n,
                int roffset, int goffset, int boffset)
{
    int r, g, b;

    r = data[i * n + 0] + roffset;
    g = data[i * n + 1] + goffset;
    b = data[i * n + 2] + boffset;

    if (r < 0) {
        r = 0;
    }
    if (g < 0) {
        g = 0;
    }
    if (b < 0) {
        b = 0;
    }
    if (r > 255) {
        r = 255;
    }
    if (g > 255) {
        g = 255;
    }
    if (b > 255) {
        b = 255;
    }

    data[i * n + 0] = (unsigned char)r;
    data[i * n + 1] = (unsigned char)g;
    data[i * n + 2] = (unsigned char)b;
}


unsigned char *
apply_palette(unsigned char *data,
              int width, int height, int depth,
              unsigned char *palette, int ncolor)
{
    int i;
    int j;
    int x, y;
    int r = 0, g = 0, b = 0;
    int rdiff, gdiff, bdiff;
    int roffset, goffset, boffset;
    int distant;
    int diff;
    int index;
    unsigned char *result;

    result = malloc(width * height);

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            i = y * width + x;
            r = data[i * depth + 0];
            g = data[i * depth + 1];
            b = data[i * depth + 2];
            diff = 256 * 256 * 3;
            index = -1;
            j = 0;
            do {
                rdiff = r - (int)palette[j * 3 + 0];
                gdiff = g - (int)palette[j * 3 + 1];
                bdiff = b - (int)palette[j * 3 + 2];
                distant = rdiff * rdiff + gdiff * gdiff + bdiff * bdiff;
                if (distant < diff) {
                    diff = distant;
                    index = j;
                }
                j++;
            } while (j != ncolor + 1);
            if (index != -1) {
                result[i] = index;
                if (ncolor > 2) {
                    roffset = (int)data[i * depth + 0] - (int)palette[index * 3 + 0];
                    goffset = (int)data[i * depth + 1] - (int)palette[index * 3 + 1];
                    boffset = (int)data[i * depth + 2] - (int)palette[index * 3 + 2];
                    if (y < height - 1) {
                        add_offset(data, i + width, depth,
                                   roffset * 5 / 16,
                                   goffset * 5 / 16,
                                   boffset * 5 / 16);
                        if (x > 1) {
                            add_offset(data, i + width - 1, depth,
                                       roffset * 3 / 16,
                                       goffset * 3 / 16,
                                       boffset * 3 / 16);
                            roffset -= roffset * 3 / 16;
                            goffset -= goffset * 3 / 16;
                            boffset -= boffset * 3 / 16;
                        }
                        if (x < width - 1) {
                            add_offset(data, i + width + 1, depth,
                                       roffset * 1 / 16,
                                       goffset * 1 / 16,
                                       boffset * 1 / 16);
                        }
                    }
                    if (x < width - 1) {
                        roffset -= roffset * 5 / 16;
                        goffset -= goffset * 5 / 16;
                        boffset -= boffset * 5 / 16;
                        roffset -= roffset * 3 / 16;
                        goffset -= goffset * 3 / 16;
                        boffset -= boffset * 3 / 16;
                        roffset -= roffset * 1 / 16;
                        goffset -= goffset * 1 / 16;
                        boffset -= boffset * 1 / 16;
                        add_offset(data, i + 1, depth,
                                   roffset,
                                   goffset,
                                   boffset);
                    }
                }
            }
        }
    }
    return result;
}

/* emacs, -*- Mode: C; tab-width: 4; indent-tabs-mode: nil -*- */
/* vim: set expandtab ts=4 : */
/* EOF */