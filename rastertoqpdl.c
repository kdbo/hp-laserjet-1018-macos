/*
 * rastertoqpdl.c — CUPS raster filter for Samsung QPDL printers
 *
 * Converts CUPS raster input to Samsung QPDL format with JBIG compression.
 *
 * Supported printers:
 *   Samsung CLP-300, CLP-310, CLP-315, CLP-325, CLP-365,
 *   Samsung CLP-600, CLP-610, CLP-620,
 *   Samsung CLX-2160, CLX-3160, CLX-3175, CLX-3185,
 *   Xerox Phaser 6110
 *
 * Model variants:
 *   0 = CLP-300/CLX-2160/CLX-3160/Xerox 6110
 *   1 = CLP-600
 *   2 = CLP-310/CLP-315/CLP-610/CLX-3175
 *   3 = CLP-325/CLP-365/CLP-620/CLX-3185
 *
 * Pipeline: PDF → cgpdftoraster (macOS built-in) → rastertoqpdl → printer
 *
 * Based on foo2qpdl from the foo2zjs project by Rick Richardson.
 * Rewritten as a native CUPS raster filter.
 *
 * License: GPL v2 or later (same as foo2zjs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include "jbig.h"
#include "qpdl.h"

/* Model variants */
#define MODEL_CLP300    0
#define MODEL_CLP600    1
#define MODEL_CLP610    2
#define MODEL_CLP620    3

/* ---------- Checksum helpers ---------- */

static int
write_cksum(void *vbuf, int len, FILE *fp)
{
    int i, cksum;
    unsigned char *buf = (unsigned char *)vbuf;

    for (cksum = 0, i = 0; i < len; ++i)
    {
        cksum += buf[i];
        putc(buf[i], fp);
    }
    return cksum;
}

static int
be32_write(FILE *fp, unsigned long value)
{
    value = be32(value);
    return write_cksum(&value, 4, fp);
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

/* QPDL JBIG options — MX=0, no ILEAVE/SMID, no DPON */
static long JbgOptions[5] = {
    0,                                          /* Order */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPBON,     /* Options */
    256,                                        /* L0 */
    0,                                          /* MX (0 for Samsung!) */
    0                                           /* MY */
};

static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 0x80000;

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertoqpdl: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertoqpdl: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertoqpdl: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- QPDL page output ---------- */

static void
start_page_init(FILE *fp, int resX, int resY, int copies, int paperCode,
                int sourceCode, int pageW, int pageH, int model, int bpp)
{
    int valw, valh;

    /* RECTYPE 0x00: page init */
    fprintf(fp, "%c", 0);
    fprintf(fp, "%c", resY / 100);
    fprintf(fp, "%c%c", copies >> 8, copies);
    fprintf(fp, "%c", paperCode);

    valw = 300 * pageW / resX;
    valh = 300 * pageH / resY;
    fprintf(fp, "%c%c", valw >> 8, valw);
    fprintf(fp, "%c%c", valh >> 8, valh);
    fprintf(fp, "%c", sourceCode);
    fprintf(fp, "%c", 0);
    /* Duplex off */
    fprintf(fp, "%c", 0);
    fprintf(fp, "%c", 0);
    fprintf(fp, "%c", 0);

    switch (model)
    {
    case MODEL_CLP610:
        fprintf(fp, "%c", 5);
        fprintf(fp, "%c%c", 1, resX / 100);
        break;
    case MODEL_CLP620:
        fprintf(fp, "%c", 5);
        fprintf(fp, "%c%c", bpp, (resX / bpp) / 100);
        break;
    default:
        fprintf(fp, "%c", 2);
        fprintf(fp, "%c%c", 1, resX / 100);
        break;
    }

    if (model == MODEL_CLP610 || model == MODEL_CLP620)
    {
        /* RECTYPE 0x13 */
        fprintf(fp, "%c", 0x13);
        fprintf(fp, "%c%c%c", 0, 0, 0);
        fprintf(fp, "%c%c", 0x23, 0x15);
        fprintf(fp, "%c%c%c%c", 0, 0, 0, 0);
        fprintf(fp, "%c%c%c%c", 0, 0, 0, 0);
        fprintf(fp, "%c", 0);
    }
}

static void
write_bih_record(FILE *fp, unsigned char *bih, int w, int h, int nbie,
                 int model)
{
    int cksum;
    int pn, i;
    /* BIH records for each plane (mono: just plane 4=K) */
    for (pn = 4, i = 0; i < nbie; ++i)
    {
        /* RECTYPE 0x0C: JBIG BIH */
        fprintf(fp, "%c", 12);
        fprintf(fp, "%c", 0);
        fprintf(fp, "%c%c", (char)((w / 8) >> 8), (char)(w / 8));
        fprintf(fp, "%c%c", 0, 128);
        fprintf(fp, "%c", pn);
        fprintf(fp, "%c", 0x13);
        be32_write(fp, 20 + 36);

        cksum = be32_write(fp, 0x39abcdef);
        cksum += be32_write(fp, 20);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += write_cksum(bih, 20, fp);
        be32_write(fp, cksum);
        if (++pn == 5) pn = 1;
    }
}

static int
write_plane(int pn, BIE_CHAIN **root, FILE *fp)
{
    BIE_CHAIN *current = *root;
    BIE_CHAIN *next;
    int len, w;
    int cksum;
    int stripe = 0;

    if (!current || !current->next || current->len != 20)
    {
        fprintf(stderr, "ERROR: rastertoqpdl: invalid JBIG data\n");
        return 1;
    }

    w = (((long)current->data[4] << 24) |
         ((long)current->data[5] << 16) |
         ((long)current->data[6] << 8) |
         (long)current->data[7]);

    for (current = (*root)->next; current && current->len;
         current = current->next)
    {
        len = current->len;
        next = current->next;
        ++stripe;

        /* RECTYPE 0x0C: JBIG data stripe */
        fprintf(fp, "%c", 12);
        fprintf(fp, "%c", stripe);
        fprintf(fp, "%c%c", (char)((w / 8) >> 8), (char)(w / 8));
        fprintf(fp, "%c%c", 0, 128);
        fprintf(fp, "%c", pn);
        fprintf(fp, "%c", 0x13);
        be32_write(fp, len + 36);

        cksum = be32_write(fp, 0x39abcdef);
        cksum += be32_write(fp, len);
        if (next && next->len)
            cksum += be32_write(fp, 0x01000000);
        else
            cksum += be32_write(fp, 0x02000000);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);
        cksum += be32_write(fp, 0);

        cksum += write_cksum(current->data, len, fp);

        be32_write(fp, cksum);
    }

    free_chain(*root);
    return 0;
}

static void
end_page(FILE *fp, int copies)
{
    /* RECTYPE 0x01: end page */
    fprintf(fp, "%c", 1);
    fprintf(fp, "%c%c", copies >> 8, copies);
}

/* ---------- PJL preamble/postamble ---------- */

static void
start_doc(FILE *fp, int mediaCode, int model)
{
    time_t now;
    struct tm *tmp;
    char datetime[14 + 1];

    static const char *strmedia[] = {
        "NORMAL", "THICK", "THIN", "BOND", "COLOR",
        "CARD", "LABEL", "ENV", "USED", "COTTON",
        "RECYCLED", "OHP", "ARCHIVE",
    };

    #define STRARY(X, A) \
        ((X) >= 0 && (X) < (int)(sizeof(A)/sizeof(A[0]))) ? A[X] : "NORMAL"

    now = time(NULL);
    tmp = localtime(&now);
    strftime(datetime, sizeof(datetime), "%Y%m%d", tmp);

    fprintf(fp, "\033%%-12345X@PJL DEFAULT SERVICEDATE=%s\r\n", datetime);
    fprintf(fp, "@PJL SET USERNAME=\"Unknown\"\r\n");
    fprintf(fp, "@PJL SET JOBNAME=\"Unknown\"\r\n");
    fprintf(fp, "@PJL SET COLORMODE=MONO\r\n");

    if (model == MODEL_CLP620)
    {
        fprintf(fp, "@PJL SET RESOLUTION=600\r\n");
        fprintf(fp, "@PJL SET BITSPERPIXEL=1\r\n");
    }

    fprintf(fp, "@PJL SET PAPERTYPE = %s\r\n", STRARY(mediaCode, strmedia));
    fprintf(fp, "@PJL ENTER LANGUAGE = QPDL\r\n");
}

static void
end_doc(FILE *fp)
{
    fprintf(fp, "%c", 9);
    fprintf(fp, "\033%%-12345X");
}

/* ---------- Mapping functions ---------- */

/*
 * Samsung QPDL paper codes (same as DMPAPER for common sizes):
 * 0=letter, 5=legal, 7=executive, 9=A4, 11=A5, 13=B5,
 * 20=env#10, 27=envDL, 28=envC5, 37=envMonarch, 21=custom
 */
static int
map_paper_code(const char *name)
{
    if (!name || !name[0])              return 0; /* letter */

    if (strcmp(name, "Letter") == 0)     return 0;
    if (strcmp(name, "Legal") == 0)      return 5;
    if (strcmp(name, "Executive") == 0)  return 7;
    if (strcmp(name, "A4") == 0)         return 9;
    if (strcmp(name, "A5") == 0)         return 11;
    if (strcmp(name, "B5") == 0)         return 13;
    if (strcmp(name, "Folio") == 0)      return 14;
    if (strcmp(name, "Env10") == 0)      return 20;
    if (strcmp(name, "EnvDL") == 0)      return 27;
    if (strcmp(name, "EnvC5") == 0)      return 28;
    if (strcmp(name, "EnvMonarch") == 0) return 37;

    return 0;
}

static int
map_media_code(const char *media)
{
    if (!media || !media[0])                return DMMEDIA_PLAIN;

    if (strcmp(media, "Plain") == 0)         return DMMEDIA_PLAIN;
    if (strcmp(media, "Thick") == 0)         return DMMEDIA_THICK;
    if (strcmp(media, "Thin") == 0)          return DMMEDIA_THIN;
    if (strcmp(media, "Bond") == 0)          return DMMEDIA_BOND;
    if (strcmp(media, "Color") == 0)         return DMMEDIA_COLOR;
    if (strcmp(media, "Cardstock") == 0)     return DMMEDIA_CARDSTOCK;
    if (strcmp(media, "Labels") == 0)        return DMMEDIA_LABELS;
    if (strcmp(media, "Envelope") == 0)      return DMMEDIA_ENVELOPE;
    if (strcmp(media, "Preprinted") == 0)    return DMMEDIA_PREPRINTED;
    if (strcmp(media, "Cotton") == 0)        return DMMEDIA_COTTON;
    if (strcmp(media, "Recycled") == 0)      return DMMEDIA_RECYCLED;
    if (strcmp(media, "Transparency") == 0)  return 11; /* OHP */

    return DMMEDIA_PLAIN;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return DMBIN_AUTO;
    case 2:  return DMBIN_MANUAL;
    case 3:  return DMBIN_MULTI;
    case 4:  return DMBIN_TRAY1;
    default: return DMBIN_AUTO;
    }
}

/* ---------- CUPS options parsing ---------- */

static void
parse_options(const char *options, int *model)
{
    cups_option_t *opts = NULL;
    int num_opts;
    const char *val;

    if (!options || !options[0])
        return;

    num_opts = cupsParseOptions(options, 0, &opts);

    val = cupsGetOption("Model", num_opts, opts);
    if (val)
    {
        int m = atoi(val);
        if (m >= 0 && m <= 3)
            *model = m;
    }

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
    int                 model = MODEL_CLP300;
    int                 first_page = 1;

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_options(argv[5], &model);

    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertoqpdl: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
    {
        fd = 0;
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertoqpdl: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY, bpp;
        int             bpl;
        unsigned char   *buf;
        unsigned char   *bitmaps[1];
        struct jbg_enc_state se;
        BIE_CHAIN       *chain = NULL;
        int             paperCode, mediaCode, sourceCode;
        unsigned int    y;
        int             invert;
        int             copies;

        page++;

        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];
        copies  = header.NumCopies > 0 ? header.NumCopies : 1;

        fprintf(stderr, "DEBUG: rastertoqpdl: page %d, %ux%u pixels, %dx%d dpi, model %d\n",
                page, cupsW, cupsH, resX, resY, model);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertoqpdl: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);

        bpp = resX / 600;
        if (bpp < 1) bpp = 1;

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType);
        sourceCode = map_source_code(header.MediaPosition);

        /* Write PJL on first page */
        if (first_page)
        {
            start_doc(stdout, mediaCode, model);
            first_page = 0;
        }

        bpl = (cupsW + 7) / 8;

        buf = calloc(bpl, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertoqpdl: cannot allocate page buffer\n");
            break;
        }

        for (y = 0; y < cupsH && !Canceled; y++)
        {
            unsigned int n = cupsRasterReadPixels(ras,
                                                  buf + (size_t)y * bpl,
                                                  cupsBpl);
            if (n == 0) break;

            if (invert)
            {
                unsigned int i;
                unsigned char *row = buf + (size_t)y * bpl;
                for (i = 0; i < cupsBpl; i++)
                    row[i] ^= 0xFF;
            }
        }

        if (Canceled)
        {
            free(buf);
            break;
        }

        *bitmaps = buf;
        jbg_enc_init(&se, cupsW, cupsH, 1, bitmaps, output_jbig, &chain);
        jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                        JbgOptions[2], JbgOptions[3], JbgOptions[4]);
        jbg_enc_out(&se);
        jbg_enc_free(&se);

        /* Page init record */
        start_page_init(stdout, resX, resY, copies, paperCode,
                        sourceCode, cupsW, cupsH, model, bpp);

        /* BIH record (for CLP300/CLP600 non-banded models) */
        if (model <= MODEL_CLP600)
            write_bih_record(stdout, chain->data, cupsW, cupsH, 1, model);

        /* JBIG data stripes */
        write_plane(4, &chain, stdout); /* plane 4=K for mono */

        end_page(stdout, copies);

        free(buf);

        fprintf(stderr, "PAGE: %d %d\n", page, copies);
    }

    end_doc(stdout);

    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return Canceled ? 1 : 0;
}
