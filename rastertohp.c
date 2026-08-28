/*
 * rastertohp.c -- CUPS raster filter for HP Color LaserJet ZJS printers
 *
 * Converts CUPS raster input to Zenographics ZJS format with HP 2600n-style
 * bitmap records (ZJT_2600N) and JBIG compression.
 *
 * Pipeline: PDF -> cgpdftoraster (macOS built-in) -> rastertohp -> printer
 *
 * Printers: HP Color LaserJet 1600, 2600n, CP1215
 *
 * Based on foo2hp from the foo2zjs project by Rick Richardson.
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
#include "zjs.h"

/* ---------- ZJS output helpers (from foo2hp.c) ---------- */

static void
chunk_write_rsvd(unsigned long type, unsigned int rsvd,
                 unsigned long items, unsigned long size, FILE *fp)
{
    ZJ_HEADER chunk;

    chunk.type = be32(type);
    chunk.items = be32(items);
    chunk.size = be32(sizeof(ZJ_HEADER) + size);
    chunk.reserved = be16(rsvd);
    chunk.signature = 0x5a5a;
    if (fwrite(&chunk, 1, sizeof(ZJ_HEADER), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertohp: chunk_write failed\n");
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
    ZJ_ITEM_UINT32 rec;

    rec.header.size = be32(sizeof(ZJ_ITEM_UINT32));
    rec.header.item = be16(item);
    rec.header.type = ZJIT_UINT32;
    rec.header.param = 0;
    rec.value = be32(value);
    if (fwrite(&rec, 1, sizeof(ZJ_ITEM_UINT32), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertohp: item_uint32_write failed\n");
        exit(1);
    }
}

static int
item_bytelut_write(unsigned short item, int size, unsigned char *p, FILE *fp)
{
    int lenpadded;
    ZJ_ITEM_HEADER hdr;
    DWORD val;

    lenpadded = 4 * ((size + 3) / 4);

    hdr.size = be32(sizeof(hdr) + 4 + lenpadded);
    hdr.item = be16(item);
    hdr.type = ZJIT_BYTELUT;
    hdr.param = 0;
    if (fp)
    {
        val = be32(size);
        if (fwrite(&hdr, sizeof(hdr), 1, fp) == 0 ||
            fwrite(&val, 4, 1, fp) == 0 ||
            fwrite(p, lenpadded, 1, fp) == 0)
        {
            fprintf(stderr, "ERROR: rastertohp: item_bytelut_write failed\n");
            exit(1);
        }
    }
    return (sizeof(hdr) + 4 + lenpadded);
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

/* JBIG encoding parameters -- match foo2hp defaults */
static long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,                                         /* Order */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, /* Options */
    128,                                                            /* L0 */
    16,                                                             /* MX */
    0                                                               /* MY */
};

/*
 * JBIG output callback -- strips the JBIG end-of-stripe marker (ff 02)
 * because the HP 2600n format embeds raw BID data without it.
 * Builds a linked list: first item is the 20-byte BIH, rest are 64KB chunks.
 */
static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 65536;
    size_t i;
    int state;
    unsigned char ch;

    /* Delete everything after ff 02 (JBIG NEWLEN marker) */
    state = 0;
    for (i = 0; i < len; ++i)
    {
        ch = start[i];
        switch (state)
        {
        case 0:
            if (ch == 0xff) state = 0xff;
            break;
        case 0xff:
            if (ch == 0x02) { len = i + 1; goto out; }
            state = 0;
            break;
        }
    }
out:

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertohp: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohp: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohp: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- HP ZJS plane output ---------- */

/*
 * write_bitmap_plane: Write a ZJT_2600N record containing JBIG BID data
 * for one stripe of one plane.
 *
 * planeNum: 1=C, 2=M, 3=Y, 4=K
 * eof: 1 if this is the last stripe for this plane
 * incry: number of lines in this stripe
 */
static int
write_bitmap_plane(int planeNum, int eof, int incry,
                   BIE_CHAIN **root, FILE *fp)
{
    BIE_CHAIN *current;
    BIE_CHAIN *next;
    int len, pad_len;
    int datasize;
    #define PADTO 4

    if (!*root || !(*root)->next || (*root)->len != 20)
    {
        fprintf(stderr, "ERROR: rastertohp: invalid JBIG data\n");
        return 1;
    }

    /* Calculate total BID data size (everything after BIH) */
    datasize = 0;
    for (current = (*root)->next; current && current->len; current = current->next)
    {
        len = current->len;
        next = current->next;
        if (!next || !next->len)
            pad_len = PADTO * ((len + PADTO - 1) / PADTO) - len;
        else
            pad_len = 0;
        datasize += len + pad_len;
    }

    if (eof)
    {
        chunk_write_rsvd(ZJT_2600N, 0x24,
                         3, datasize + 3 * sizeof(ZJ_ITEM_UINT32), fp);
        item_uint32_write(ZJI_PLANE, planeNum, fp);
        item_uint32_write(ZJI_INCRY, incry, fp);
        item_uint32_write(0x67, 1, fp);
    }
    else
    {
        chunk_write_rsvd(ZJT_2600N, 0xc,
                         1, datasize + 1 * sizeof(ZJ_ITEM_UINT32), fp);
        item_uint32_write(ZJI_PLANE, planeNum, fp);
    }

    for (current = (*root)->next; current && current->len; current = current->next)
    {
        int i;

        len = current->len;
        next = current->next;
        if (!next || !next->len)
            pad_len = PADTO * ((len + PADTO - 1) / PADTO) - len;
        else
            pad_len = 0;

        fwrite(current->data, 1, len, fp);
        for (i = 0; i < pad_len; i++)
            fputc(0, fp);
    }

    free_chain(*root);
    return 0;
}

/* ---------- Document start/end ---------- */

static void
start_doc(FILE *fp)
{
    char header[4] = "JZJZ";
    int nitems, size;

    if (fwrite(header, 1, sizeof(header), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertohp: write header failed\n");
        exit(1);
    }

    nitems = 3;
    size = nitems * sizeof(ZJ_ITEM_UINT32);

    chunk_write_rsvd(ZJT_START_DOC, 0x24, nitems, size, fp);
    item_uint32_write(ZJI_DMCOLLATE, 0, fp);
    item_uint32_write(ZJI_DMDUPLEX, DMDUPLEX_OFF, fp);
    item_uint32_write(ZJI_PAGECOUNT, 0, fp);
}

static void
end_doc(FILE *fp)
{
    chunk_write(ZJT_END_DOC, 0, 0, fp);
}

/* ---------- Mapping functions ---------- */

static int
map_paper_code(const char *name)
{
    if (!name || !name[0])           return DMPAPER_LETTER;
    if (strcmp(name, "Letter") == 0)  return DMPAPER_LETTER;
    if (strcmp(name, "Legal") == 0)   return DMPAPER_LEGAL;
    if (strcmp(name, "Executive") == 0) return DMPAPER_EXECUTIVE;
    if (strcmp(name, "A4") == 0)     return DMPAPER_A4;
    if (strcmp(name, "A5") == 0)     return DMPAPER_A5;
    if (strcmp(name, "B5") == 0)     return DMPAPER_B5;
    if (strcmp(name, "Env10") == 0)  return DMPAPER_ENV_10;
    if (strcmp(name, "EnvDL") == 0)  return DMPAPER_ENV_DL;
    if (strcmp(name, "EnvC5") == 0)  return DMPAPER_ENV_C5;
    if (strcmp(name, "EnvMonarch") == 0) return DMPAPER_ENV_MONARCH;
    return DMPAPER_LETTER;
}

static int
map_media_code(const char *media)
{
    if (!media || !media[0])            return DMMEDIA_STANDARD;
    if (strcmp(media, "Plain") == 0)     return DMMEDIA_STANDARD;
    if (strcmp(media, "Transparency") == 0) return DMMEDIA_TRANSPARENCY;
    if (strcmp(media, "Envelope") == 0)  return 267;
    if (strcmp(media, "Labels") == 0)    return 265;
    if (strcmp(media, "Heavy") == 0)     return 262;
    if (strcmp(media, "Letterhead") == 0) return 513;
    if (strcmp(media, "Bond") == 0)      return 260;
    if (strcmp(media, "Recycled") == 0)  return 516;
    return DMMEDIA_STANDARD;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return 2;
    case 2:  return 1;
    case 4:  return DMBIN_MANUAL;
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
            fprintf(stderr, "ERROR: rastertohp: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
        fd = 0;

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertohp: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    start_doc(stdout);

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY, bpp;
        int             jbigW, w16;
        int             bpl, bpl16;
        unsigned char   *buf;
        unsigned int    y;
        int             invert;
        int             paperCode, mediaCode, sourceCode, copies;
        int             nitems, size_items, i;
        DWORD           bih[5];

        page++;
        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];
        copies  = header.NumCopies > 0 ? header.NumCopies : 1;

        fprintf(stderr, "DEBUG: rastertohp: page %d, %ux%u pixels, %dx%d dpi\n",
                page, cupsW, cupsH, resX, resY);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertohp: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);
        bpp = resX / 600;
        if (bpp < 1) bpp = 1;

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType);
        sourceCode = map_source_code(header.MediaPosition);

        jbigW = (cupsW + 127) & ~127;
        bpl   = (jbigW + 7) / 8;
        bpl16 = (bpl + 15) & ~15;
        w16   = jbigW;

        buf = calloc(bpl16, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertohp: malloc failed\n");
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

        /* START_PAGE */
        nitems = 13;
        chunk_write_rsvd(ZJT_START_PAGE, 0x9c,
                         nitems, nitems * sizeof(ZJ_ITEM_UINT32), stdout);
        item_uint32_write(ZJI_PLANE,           1,          stdout);
        item_uint32_write(ZJI_DMPAPER,         paperCode,  stdout);
        item_uint32_write(ZJI_DMCOPIES,        copies,     stdout);
        item_uint32_write(ZJI_DMDEFAULTSOURCE, sourceCode, stdout);
        item_uint32_write(ZJI_DMMEDIATYPE,     mediaCode,  stdout);
        item_uint32_write(ZJI_NBIE,            1,          stdout);
        item_uint32_write(ZJI_RESOLUTION_X,    resX,       stdout);
        item_uint32_write(ZJI_RESOLUTION_Y,    resY,       stdout);
        item_uint32_write(ZJI_RASTER_X,        w16 * bpp,  stdout);
        item_uint32_write(ZJI_RASTER_Y,        cupsH,      stdout);
        item_uint32_write(ZJI_VIDEO_BPP,       bpp,        stdout);
        item_uint32_write(ZJI_VIDEO_X,         w16,        stdout);
        item_uint32_write(ZJI_VIDEO_Y,         cupsH,      stdout);

        /* Bitmap header (ZJT_2600N with BIH) */
        nitems = 7;
        size_items = nitems * sizeof(ZJ_ITEM_UINT32);
        ++nitems;
        bih[0] = 1 << 8;
        bih[1] = w16 * bpp;
        bih[2] = 100;
        bih[3] = 128;
        bih[4] = (16 << 24)
                 | ((JBG_ILEAVE | JBG_SMID) << 8)
                 | ((JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON) << 0);
        for (i = 0; i < 5; ++i)
            bih[i] = be32(bih[i]);
        size_items += item_bytelut_write(0, 20, (unsigned char *)bih, NULL);

        chunk_write_rsvd(ZJT_2600N, 0x74, nitems, size_items, stdout);
        item_uint32_write(ZJI_BITMAP_TYPE,   1,         stdout);
        item_uint32_write(ZJI_BITMAP_PIXELS, w16 * bpp, stdout);
        item_uint32_write(ZJI_BITMAP_STRIDE, w16 * bpp, stdout);
        item_uint32_write(ZJI_INCRY,         100,       stdout);
        item_uint32_write(ZJI_BITMAP_BPP,    1,         stdout);
        item_uint32_write(ZJI_VIDEO_BPP,     bpp,       stdout);
        item_uint32_write(ZJI_PLANE,         4,         stdout);
        item_bytelut_write(ZJI_JBIG_BIH, 20, (unsigned char *)bih, stdout);

        /* 100-line stripes */
        for (y = 0; y < cupsH; y += 100)
        {
            struct jbg_enc_state se;
            BIE_CHAIN       *chain = NULL;
            unsigned char   *bitmaps[1];
            int             lines, eof;

            lines = cupsH - y;
            if (lines > 100) lines = 100;
            eof = (y + 100) >= cupsH;
            bitmaps[0] = buf + (size_t)y * bpl16;

            jbg_enc_init(&se, w16 * bpp, lines, 1, bitmaps,
                         output_jbig, &chain);
            jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                            JbgOptions[2], JbgOptions[3], JbgOptions[4]);
            jbg_enc_out(&se);
            jbg_enc_free(&se);
            write_bitmap_plane(4, eof, lines, &chain, stdout);
        }

        /* END_PAGE with HP dot count items */
        nitems = 8;
        chunk_write_rsvd(ZJT_END_PAGE, 0x60,
                         nitems, nitems * sizeof(ZJ_ITEM_UINT32), stdout);
        item_uint32_write(ZJI_HP_CDOTS,  0, stdout);
        item_uint32_write(ZJI_HP_MDOTS,  0, stdout);
        item_uint32_write(ZJI_HP_YDOTS,  0, stdout);
        item_uint32_write(ZJI_HP_KDOTS,  1, stdout);
        item_uint32_write(ZJI_HP_CWHITE, 0, stdout);
        item_uint32_write(ZJI_HP_MWHITE, 0, stdout);
        item_uint32_write(ZJI_HP_YWHITE, 0, stdout);
        item_uint32_write(ZJI_HP_KWHITE, 1, stdout);

        free(buf);
        fprintf(stderr, "PAGE: %d %d\n", page, copies);
    }

    end_doc(stdout);
    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return Canceled ? 1 : 0;
}
