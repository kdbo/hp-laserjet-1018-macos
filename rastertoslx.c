/*
 * rastertoslx.c -- CUPS raster filter for Lexmark SLX printers
 *
 * Converts CUPS raster input to Software Imaging K.K. SLX-stream format
 * with JBIG compression. SLX is closely related to ZjStream but with
 * different item numbers and a different magic header.
 *
 * Pipeline: PDF -> cgpdftoraster (macOS built-in) -> rastertoslx -> printer
 *
 * Printers: Lexmark C500
 *
 * Based on foo2slx from the foo2zjs project by Rick Richardson.
 * Rewritten as a native CUPS raster filter by Anish.
 *
 * License: GPL v2 or later (same as foo2zjs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include "jbig.h"
#include "slx.h"

/* ---------- SLX output helpers (from foo2slx.c) ---------- */

static void
chunk_write_rsvd(unsigned long type, unsigned int rsvd,
                 unsigned long items, unsigned long size, FILE *fp)
{
    SL_HEADER chunk;

    chunk.type = be32(type);
    chunk.items = be32(items);
    chunk.size = be32(sizeof(SL_HEADER) + size);
    chunk.reserved = be16(rsvd);
    chunk.signature = 0xa5a5;   /* SLX uses 0xa5a5, not 0x5a5a */
    if (fwrite(&chunk, 1, sizeof(SL_HEADER), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertoslx: chunk_write failed\n");
        exit(1);
    }
}

static void
chunk_write(unsigned long type, unsigned long items, unsigned long size,
            FILE *fp)
{
    chunk_write_rsvd(type, 0, items, size, fp);
}

static void
item_uint32_write(unsigned short item, unsigned long value, FILE *fp)
{
    SL_ITEM_UINT32 rec;

    rec.header.size = be32(sizeof(SL_ITEM_UINT32));
    rec.header.item = be16(item);
    rec.header.type = SLIT_UINT32;
    rec.header.param = 0;
    rec.value = be32(value);
    if (fwrite(&rec, 1, sizeof(SL_ITEM_UINT32), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertoslx: item_uint32_write failed\n");
        exit(1);
    }
}

static void
item_int32_write_pad(unsigned short item, long value, int pad, FILE *fp)
{
    SL_ITEM_INT32 rec;

    rec.header.size = be32(sizeof(SL_ITEM_INT32) + pad);
    rec.header.item = be16(item);
    rec.header.type = SLIT_INT32;
    rec.header.param = 0;
    rec.value = be32(value);
    if (fwrite(&rec, 1, sizeof(SL_ITEM_INT32), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertoslx: item_int32_write_pad failed\n");
        exit(1);
    }
    while (pad--)
        fputc(0, fp);
}

/* ---------- JBIG compressed data chain ---------- */

typedef struct _BIE_CHAIN {
    unsigned char   *data;
    size_t          len;
    struct _BIE_CHAIN *next;
} BIE_CHAIN;

static void
free_chain(BIE_CHAIN *chain)
{
    BIE_CHAIN *next;
    next = chain;
    while ((chain = next))
    {
        next = chain->next;
        if (chain->data)
            free(chain->data);
        free(chain);
    }
}

/*
 * JBIG encoding parameters -- match foo2slx defaults.
 * NOTE: foo2slx uses Order=0 (no ILEAVE/SMID), Options=DELAY_AT|TPBON,
 * L0=128, MX=0 (not 16). This differs from the other foo2zjs variants.
 */
static long JbgOptions[5] = {
    0,                              /* Order: no ILEAVE, no SMID */
    JBG_DELAY_AT | JBG_TPBON,      /* Options */
    128,                            /* L0 */
    0,                              /* MX (not 16!) */
    0                               /* MY */
};

/*
 * JBIG output callback -- foo2slx uses a single huge buffer (20000000)
 * for the entire JBIG output. We replicate this to match printer expectations.
 */
static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 20000000;

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertoslx: malloc failed\n");
            exit(1);
        }
        (*root)->data = NULL;
        (*root)->next = NULL;
        (*root)->len = 0;
        size = 20;
    }

    current = *root;
    while (current->next)
        current = current->next;

    while (len > 0)
    {
        int amt, left;

        if (!current->data)
        {
            current->data = malloc(size);
            if (!current->data)
            {
                fprintf(stderr, "ERROR: rastertoslx: malloc failed\n");
                exit(1);
            }
        }

        left = size - current->len;
        amt = (len > (size_t)left) ? left : (int)len;
        memcpy(current->data + current->len, start, amt);
        current->len += amt;
        len -= amt;
        start += amt;

        if (current->len == (size_t)size)
        {
            current->next = malloc(sizeof(BIE_CHAIN));
            if (!current->next)
            {
                fprintf(stderr, "ERROR: rastertoslx: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- SLX plane output ---------- */

static int
write_plane(int planeNum, BIE_CHAIN **root, FILE *fp)
{
    BIE_CHAIN *current = *root;
    BIE_CHAIN *next;
    int len, pad_len;
    #define PADTO 4

    if (!current || !current->next || current->len != 20)
    {
        fprintf(stderr, "ERROR: rastertoslx: invalid JBIG data\n");
        return 1;
    }

    /* Write BIH record */
    chunk_write(SLT_JBIG_BIH, 0, current->len, fp);
    if (fwrite(current->data, 1, current->len, fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertoslx: write BIH failed\n");
        return 1;
    }

    /* Write BID records */
    for (current = (*root)->next; current && current->len; current = current->next)
    {
        int i;

        len = current->len;
        next = current->next;
        if (!next || !next->len)
            pad_len = PADTO * ((len + PADTO - 1) / PADTO) - len;
        else
            pad_len = 0;

        chunk_write(SLT_JBIG_BID, 0, len + pad_len, fp);
        if (fwrite(current->data, 1, len, fp) == 0)
        {
            fprintf(stderr, "ERROR: rastertoslx: write BID failed\n");
            return 1;
        }
        for (i = 0; i < pad_len; i++)
            fputc(0, fp);
    }

    free_chain(*root);

    /* End JBIG */
    chunk_write(SLT_END_JBIG, 0, 0, fp);
    return 0;
}

/* ---------- Document start/end ---------- */

static void
start_doc(FILE *fp)
{
    char header[4] = "\245SLX";   /* 0xa5, 'S', 'L', 'X' */
    int nitems, size;
    int pad1 = 12;

    if (fwrite(header, 1, sizeof(header), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertoslx: write header failed\n");
        exit(1);
    }

    nitems = 12;
    size = nitems * sizeof(SL_ITEM_UINT32) + pad1;

    chunk_write(SLT_START_DOC, nitems, size, fp);

    item_uint32_write(SLI_PAGECOUNT,        -1,     fp);
    item_uint32_write(SLI_DMDUPLEX,          0,     fp);
    item_uint32_write(SLI_DMCOLLATE,         0,     fp);
    item_int32_write_pad(0x03,               0, pad1, fp);
    item_int32_write_pad(0x04,               0, 0,   fp);
    item_uint32_write(0x05,                  0,     fp);
    item_uint32_write(0x06,                  0,     fp);
    item_uint32_write(0x07,                  1,     fp);
    item_uint32_write(0x08,                  0,     fp);
    item_uint32_write(0x09,                  0,     fp);
    item_uint32_write(SLI_COUNT,             1,     fp);
    item_uint32_write(0x0e,                  0,     fp);
}

static void
end_doc(FILE *fp)
{
    chunk_write(SLT_END_DOC, 0, 0, fp);
}

/* ---------- Page start/end ---------- */

static void
start_page(unsigned long w, unsigned long h, int nbie,
           int paperCode, int copies, int sourceCode, int mediaCode,
           int resX, int resY, int realWidth, int bpp,
           FILE *fp)
{
    int nitems = 14;

    chunk_write(SLT_START_PAGE,
                nitems, nitems * sizeof(SL_ITEM_UINT32), fp);

    item_uint32_write(SLI_DMPAPER,          paperCode,     fp);
    item_uint32_write(SLI_CUSTOM_X,         0,             fp);
    item_uint32_write(SLI_CUSTOM_Y,         0,             fp);
    item_uint32_write(SLI_DMCOPIES,         copies,        fp);
    item_uint32_write(SLI_DMDEFAULTSOURCE,  sourceCode,    fp);
    item_uint32_write(SLI_DMMEDIATYPE,      mediaCode,     fp);
    item_uint32_write(SLI_NBIE,             (nbie == 4) ? 14 : 0, fp);
    item_uint32_write(SLI_RESOLUTION_X,     resX,          fp);
    item_uint32_write(SLI_RESOLUTION_Y,     resY,          fp);
    item_uint32_write(SLI_RASTER_X,         realWidth,     fp);
    item_uint32_write(SLI_RASTER_Y,         h,             fp);
    item_uint32_write(SLI_VIDEO_X,          realWidth / bpp, fp);
    item_uint32_write(SLI_VIDEO_Y,          h,             fp);
    item_uint32_write(0x10f,                1,             fp);
}

static void
end_page(FILE *fp)
{
    chunk_write(SLT_END_PAGE, 0, 0, fp);
}

/* ---------- Mapping functions ---------- */

/*
 * SLX paper codes differ from ZJS: Letter=6, A4=2
 */
static int
map_paper_code(const char *name)
{
    if (!name || !name[0])           return 6;  /* DMPAPER_LETTER in SLX */
    if (strcmp(name, "Letter") == 0)  return 6;
    if (strcmp(name, "Legal") == 0)   return 9;
    if (strcmp(name, "Executive") == 0) return 8;
    if (strcmp(name, "A4") == 0)     return 2;
    if (strcmp(name, "A5") == 0)     return 11;
    if (strcmp(name, "B5") == 0)     return 4;
    if (strcmp(name, "Env10") == 0)  return 10;
    if (strcmp(name, "EnvDL") == 0)  return 11;
    return 6;
}

static int
map_media_code(const char *media)
{
    if (!media || !media[0])            return DMMEDIA_STANDARD;
    if (strcmp(media, "Plain") == 0)     return DMMEDIA_STANDARD;
    if (strcmp(media, "Transparency") == 0) return DMMEDIA_TRANSPARENCY;
    if (strcmp(media, "Labels") == 0)    return 2;
    if (strcmp(media, "Heavy") == 0)     return 3;
    if (strcmp(media, "Envelope") == 0)  return 4;
    if (strcmp(media, "Light") == 0)     return 5;
    return DMMEDIA_STANDARD;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return DMBIN_CASSETTE1;
    default: return DMBIN_AUTO;
    }
}

/* ---------- CUPS options parsing ---------- */

static void
parse_options(const char *options, int *density, int *economode)
{
    cups_option_t *opts = NULL;
    int num_opts;
    const char *val;

    if (!options || !options[0])
        return;

    num_opts = cupsParseOptions(options, 0, &opts);

    val = cupsGetOption("Density", num_opts, opts);
    if (val)
    {
        int d = atoi(val);
        if (d >= 1 && d <= 5) *density = d;
    }

    val = cupsGetOption("Quality", num_opts, opts);
    if (val && strcmp(val, "draft") == 0) *economode = 1;

    val = cupsGetOption("EconoMode", num_opts, opts);
    if (val && strcmp(val, "on") == 0) *economode = 1;

    cupsFreeOptions(num_opts, opts);
}

/* ---------- Signal handling ---------- */

static volatile int Canceled = 0;

static void
cancel_handler(int sig)
{
    (void)sig;
    Canceled = 1;
}

/* ---------- Main ---------- */

int
main(int argc, char *argv[])
{
    cups_raster_t       *ras;
    cups_page_header2_t header;
    int                 fd;
    int                 page = 0;
    int                 density = 3;
    int                 economode = 0;

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_options(argv[5], &density, &economode);
    (void)density;
    (void)economode;

    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertoslx: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
        fd = 0;

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertoslx: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    start_doc(stdout);

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY, bpp;
        int             jbigW, realWidth;
        int             bpl, bpl16;
        unsigned char   *buf;
        unsigned char   *bitmaps[1];
        struct jbg_enc_state se;
        BIE_CHAIN       *chain = NULL;
        unsigned int    y;
        int             invert;
        int             paperCode, mediaCode, sourceCode, copies;

        page++;
        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];
        copies  = header.NumCopies > 0 ? header.NumCopies : 1;

        fprintf(stderr, "DEBUG: rastertoslx: page %d, %ux%u pixels, %dx%d dpi\n",
                page, cupsW, cupsH, resX, resY);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertoslx: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);

        /* Lexmark C500 default is 1200x600 */
        bpp = resX / 600;
        if (bpp < 1) bpp = 1;

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType);
        sourceCode = map_source_code(header.MediaPosition);

        /* Width padding for JBIG */
        realWidth = cupsW;
        jbigW = (cupsW + 127) & ~127;
        bpl   = (jbigW + 7) / 8;
        bpl16 = (bpl + 15) & ~15;

        buf = calloc(bpl16, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertoslx: malloc failed\n");
            break;
        }

        for (y = 0; y < cupsH && !Canceled; y++)
        {
            unsigned int n = cupsRasterReadPixels(ras,
                                buf + (size_t)y * bpl16, cupsBpl);
            if (n == 0) break;
            if (invert)
            {
                unsigned int idx;
                unsigned char *row = buf + (size_t)y * bpl16;
                for (idx = 0; idx < cupsBpl; idx++)
                    row[idx] ^= 0xFF;
            }
        }

        if (Canceled) { free(buf); break; }

        /* JBIG encode entire page */
        *bitmaps = buf;
        jbg_enc_init(&se, jbigW, cupsH, 1, bitmaps, output_jbig, &chain);
        jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                        JbgOptions[2], JbgOptions[3], JbgOptions[4]);
        jbg_enc_out(&se);
        jbg_enc_free(&se);

        /* Write SLX page */
        start_page(jbigW, cupsH, 1,
                   paperCode, copies, sourceCode, mediaCode,
                   resX, resY, realWidth, bpp,
                   stdout);
        write_plane(4, &chain, stdout);
        end_page(stdout);

        free(buf);

        fprintf(stderr, "PAGE: %d %d\n", page, copies);
    }

    end_doc(stdout);

    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return Canceled ? 1 : 0;
}
