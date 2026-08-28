/*
 * rastertolava.c -- CUPS raster filter for Konica Minolta LAVAFLOW printers
 *
 * Converts CUPS raster input to Zenographics LAVAFLOW format with JBIG
 * compression. Designed to work with macOS's built-in cgpdftoraster
 * filter, eliminating the need for Ghostscript entirely.
 *
 * Pipeline: PDF -> cgpdftoraster (macOS built-in) -> rastertolava -> printer
 *
 * Supported printers:
 *   Model 0 (2530DL): Konica Minolta magicolor 2490MF, 2530DL,
 *                      Xerox Phaser 6115MFP
 *   Model 1 (2480MF): Konica Minolta magicolor 2480MF
 *   Model 2 (1600W):  Konica Minolta magicolor 1600W, 1680MF, 1690MF,
 *                      4690MF, Oki C110, Xerox Phaser 6121MFP
 *
 * Based on foo2lava from the foo2zjs project by Rick Richardson.
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
#include <time.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include "jbig.h"

/* LAVAFLOW has no header file -- all types are defined inline */

/* ---------- Model codes ---------- */

#define MODEL_2530DL   0
#define MODEL_2480MF   1
#define MODEL_1600W    2

/* ---------- JBIG compressed data chain ---------- */

typedef struct _BIE_CHAIN {
    unsigned char   *data;
    size_t          len;
    struct _BIE_CHAIN *next;
} BIE_CHAIN;

static int Model = MODEL_2530DL;

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

/* JBIG encoding parameters -- must match what the printer expects */
static long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,                                         /* Order */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, /* Options */
    128,                                                            /* L0 */
    16,                                                             /* MX */
    0                                                               /* MY */
};

/*
 * JBIG output callback -- builds a linked list of compressed data.
 * First item is always the 20-byte BIH. Subsequent items are 65536 bytes
 * max (or 32768 for Model 1 / 2480MF).
 */
static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 65536;

    if (Model == MODEL_2480MF)
        size = 32768;

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertolava: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertolava: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertolava: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- Dot counting (for Models 0,2 plane statistics) ---------- */

static int BlackOnes[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

static int
compute_image_dots(int w, int h, unsigned char *bitmap, int bpl)
{
    int dots = 0;
    int x, y;
    int bytew = (w + 7) / 8;

    for (y = 0; y < h; ++y)
        for (x = 0; x < bytew; ++x)
            dots += BlackOnes[bitmap[y * bpl + x]];
    return dots;
}

/* ---------- LAVAFLOW write_plane ---------- */

static int
write_plane(int planeNum, BIE_CHAIN **root, FILE *fp,
            int totalDots, int blackDots)
{
    BIE_CHAIN *current = *root;
    BIE_CHAIN *next;
    int len;
    #define PAD 32

    if (!current)
    {
        fprintf(stderr, "ERROR: rastertolava: no JBIG data\n");
        return 1;
    }
    if (!current->next)
    {
        fprintf(stderr, "ERROR: rastertolava: no JBIG header\n");
        return 1;
    }
    if (current->len != 20)
    {
        fprintf(stderr, "ERROR: rastertolava: wrong BIH length\n");
        return 1;
    }

    if (Model == MODEL_2480MF)
    {
        int w, h, x = 0;

        switch (planeNum)
        {
        case 1: x = 0x00FFFF; break;
        case 2: x = 0xFF00FF; break;
        case 3: x = 0xFFFF00; break;
        }
        fprintf(fp, "RasterObject.Planes=%06X,0,0,0,0,0,0;", x);

        w = (((long)current->data[ 4] << 24)
            | ((long)current->data[ 5] << 16)
            | ((long)current->data[ 6] <<  8)
            | (long)current->data[ 7]);
        h = (((long)current->data[ 8] << 24)
            | ((long)current->data[ 9] << 16)
            | ((long)current->data[10] <<  8)
            | (long)current->data[11]);
        fprintf(fp, "RasterObject.Width=%d;", w);
        fprintf(fp, "RasterObject.Height=%d;", h);
    }

    /* Write BIH and data blocks */
    for (current = *root; current && current->len; current = current->next)
    {
        if (current == *root)
        {
            /* BIH (20 bytes) */
            switch (Model)
            {
            case MODEL_1600W:
            case MODEL_2530DL:
                fprintf(fp, "\033*b20V");
                fwrite(current->data, 1, current->len, fp);
                break;
            case MODEL_2480MF:
                fprintf(fp, "RasterObject.Data#%d=", (int)current->len);
                fwrite(current->data, 1, current->len, fp);
                fprintf(fp, ";");
                break;
            }
        }
        else
        {
            /* Data blocks */
            int i;
            int pad;

            len = current->len;
            next = current->next;
            if (!next || !next->len)
                pad = PAD * ((len + PAD - 1) / PAD) - len;
            else
                pad = 0;

            switch (Model)
            {
            case MODEL_1600W:
            case MODEL_2530DL:
                fprintf(fp, "\033*b%d%s", len + pad,
                        (next && next->len) ? "V" : "W");
                fwrite(current->data, 1, len, fp);
                for (i = 0; i < pad; i++)
                    putc(0, fp);
                break;
            case MODEL_2480MF:
                fprintf(fp, "RasterObject.Data#%d=", len + pad);
                fwrite(current->data, 1, len, fp);
                for (i = 0; i < pad; i++)
                    putc(0, fp);
                fprintf(fp, ";");
                break;
            }
        }
    }

    free_chain(*root);

    /* Dot statistics for Models 0,2 */
    if (Model == MODEL_2530DL || Model == MODEL_1600W)
    {
        switch (planeNum)
        {
        case 0: case 4:
            fprintf(fp, "\033*x%dK", blackDots);
            fprintf(fp, "\033*x%dW", totalDots - blackDots);
            break;
        case 1:
            fprintf(fp, "\033*x%dC", blackDots);
            fprintf(fp, "\033*x%dZ", totalDots - blackDots);
            break;
        case 2:
            fprintf(fp, "\033*x%dM", blackDots);
            fprintf(fp, "\033*x%dV", totalDots - blackDots);
            break;
        case 3:
            fprintf(fp, "\033*x%dY", blackDots);
            fprintf(fp, "\033*x%dU", totalDots - blackDots);
            break;
        }
    }

    return 0;
}

/* ---------- LAVAFLOW document start/end ---------- */

static void
start_doc(FILE *fp, const char *filename, const char *username,
          int duplex, int resX, int resY, int copies)
{
    char buf[64];
    time_t now;
    struct tm *tmp;

    switch (Model)
    {
    case MODEL_1600W:
    case MODEL_2530DL:
        now = time(NULL);
        tmp = localtime(&now);
        strftime(buf, sizeof(buf), "%m/%d/%Y", tmp);

        fprintf(fp, "\033%%-12345X@PJL JOB NAME=\"%s\"\n",
                filename ? filename : "stdin");
        fprintf(fp, "\033%%-12345X@PJL JOB USERNAME=\"%s\"\n",
                username ? username : "");
        fprintf(fp, "\033%%-12345X@PJL JOB TIMESTAMP=\"%s\"\n", buf);
        fprintf(fp, "\033%%-12345X@PJL JOB OSINFO=\"macOS\"\n");
        fprintf(fp, "\033%%-12345X@PJL ENTER LANGUAGE=LAVAFLOW\n");
        fprintf(fp, "\033E");
        fprintf(fp, "\033&l%dS", duplex - 1);
        fprintf(fp, "\033&l%dG", 0);
        fprintf(fp, "\033&u%dD", resX);
        fprintf(fp, "\033&l%dX", copies);
        fprintf(fp, "\033&x%dX", 1);
        break;
    case MODEL_2480MF:
        fprintf(fp, "Event=StartOfJob;");
        fprintf(fp, "OSVersion=macOS;");
        fprintf(fp, "DrvVersion=2.0.1410.0;");
        fprintf(fp, "Resolution=%dx%d;", resX, resY);
        fprintf(fp, "RasterObject.Compression=JBIG;");
        fprintf(fp, "Sides=%sSided;", (duplex - 1) ? "Two" : "One");
        break;
    }
}

static void
end_doc(FILE *fp)
{
    switch (Model)
    {
    case MODEL_1600W:
    case MODEL_2530DL:
        fprintf(fp, "\033E");
        fprintf(fp, "\033%%-12345X");
        break;
    case MODEL_2480MF:
        fprintf(fp, "Event=EndOfJob;");
        break;
    }
}

/* ---------- LAVAFLOW page start/end ---------- */

static void
start_page(FILE *fp, int resX, int resY,
           unsigned long w, unsigned long h,
           int paperCode, const char *paperStr,
           int sourceCode, int mediaCode, const char *mediaStr)
{
    int nbie = 1;  /* mono: 1 plane */
    int i;

    switch (Model)
    {
    case MODEL_1600W:
    case MODEL_2530DL:
        fprintf(fp, "\033&l%dO", 0);
        fprintf(fp, "\033*r%dU", 1);           /* mono: 1 */
        fprintf(fp, "\033*g%dW", 8);           /* mono: 8 bytes config */
        putc(2, fp);                           /* format */
        putc(nbie, fp);                        /* planes */
        for (i = 0; i < nbie; ++i)
        {
            putc(resX >> 8, fp);
            putc(resX & 0xFF, fp);
            putc(resY >> 8, fp);
            putc(resY & 0xFF, fp);
            putc(0, fp);
            putc(2, fp);
        }
        fprintf(fp, "\033*b%dM", 1234);
        fprintf(fp, "\033&l%dA", paperCode);
        fprintf(fp, "\033&l%dH", sourceCode);
        fprintf(fp, "\033&l%dM", mediaCode);
        fprintf(fp, "\033&l%dE", 0);
        fprintf(fp, "\033*r%dS", (int)w);
        fprintf(fp, "\033*r%dT", (int)h);
        fprintf(fp, "\033&l%dU", 0);
        fprintf(fp, "\033&l%dZ", 0);
        fprintf(fp, "\033*p%dX", resX / 6);
        fprintf(fp, "\033*p%dY", resX / 6);
        fprintf(fp, "\033*r1A");
        break;
    case MODEL_2480MF:
        fprintf(fp, "MediaSize=%s;", paperStr);
        fprintf(fp, "MediaType=%s;", mediaStr);
        fprintf(fp, "MediaInputTrayCheck=top;");
        fprintf(fp, "RasterObject.BitsPerPixel=1;");
        break;
    }
}

static void
end_page(FILE *fp)
{
    switch (Model)
    {
    case MODEL_1600W:
    case MODEL_2530DL:
        fprintf(fp, "\033*rC");
        fprintf(fp, "\033&l0H");
        break;
    case MODEL_2480MF:
        fprintf(fp, "Event=EndOfPage;");
        break;
    }
}

/* ---------- Paper/media/source mapping (Models 0,2) ---------- */

static int
map_paper_code(const char *name)
{
    if (!name || !name[0])
        return 1; /* letter */

    if (strcmp(name, "Letter") == 0)       return 1;
    if (strcmp(name, "Legal") == 0)        return 5;
    if (strcmp(name, "A4") == 0)           return 6;
    if (strcmp(name, "Executive") == 0)    return 2;
    if (strcmp(name, "A5") == 0)           return 26;
    if (strcmp(name, "B5") == 0)           return 100;
    if (strcmp(name, "Custom") == 0)       return 101;
    if (strcmp(name, "Env10") == 0)        return 80;
    if (strcmp(name, "EnvDL") == 0)        return 81;
    if (strcmp(name, "EnvC5") == 0)        return 91;
    if (strcmp(name, "EnvMonarch") == 0)   return 83;

    return 1; /* default: letter */
}

/* Paper string mapping for Model 1 (2480MF) */
static const char *
map_paper_str(const char *name)
{
    if (!name || !name[0])
        return "na_letter_8.5x11in";

    if (strcmp(name, "Letter") == 0)       return "na_letter_8.5x11in";
    if (strcmp(name, "Legal") == 0)        return "na_legal_8.5x14in";
    if (strcmp(name, "A4") == 0)           return "iso_a4_210x297mm";
    if (strcmp(name, "Executive") == 0)    return "na_executive_7.25x10.5in";
    if (strcmp(name, "A5") == 0)           return "iso_a5_148x210mm";
    if (strcmp(name, "B5") == 0)           return "jis_b5_182x257mm";
    if (strcmp(name, "Env10") == 0)        return "na_number-10_4.125x9.5in";
    if (strcmp(name, "EnvDL") == 0)        return "iso_dl_110x220mm";
    if (strcmp(name, "EnvC5") == 0)        return "iso_c5_162x229mm";
    if (strcmp(name, "EnvMonarch") == 0)   return "na_monarch_3.875x7.5in";
    if (strcmp(name, "Custom") == 0)       return "custom";

    return "na_letter_8.5x11in";
}

static int
map_media_code(const char *media)
{
    if (!media || !media[0])
        return 1; /* standard/plain */

    if (strcmp(media, "Plain") == 0)         return 1;
    if (strcmp(media, "Transparency") == 0)  return 4;
    if (strcmp(media, "ThickStock") == 0)    return 20;
    if (strcmp(media, "Envelope") == 0)      return 22;
    if (strcmp(media, "Letterhead") == 0)    return 23;
    if (strcmp(media, "Postcard") == 0)      return 25;
    if (strcmp(media, "Labels") == 0)        return 26;
    if (strcmp(media, "Recycled") == 0)      return 27;
    if (strcmp(media, "Glossy") == 0)        return 28;

    return 1; /* default: plain */
}

/* Media string mapping for Model 1 (2480MF) */
static const char *
map_media_str(const char *media)
{
    if (!media || !media[0])
        return "plain";

    if (strcmp(media, "Plain") == 0)         return "plain";
    if (strcmp(media, "Transparency") == 0)  return "transparency";
    if (strcmp(media, "ThickStock") == 0)    return "thick_stock";
    if (strcmp(media, "Envelope") == 0)      return "envelope";
    if (strcmp(media, "Letterhead") == 0)    return "letterhead";
    if (strcmp(media, "Postcard") == 0)      return "postcard";
    if (strcmp(media, "Labels") == 0)        return "labels";
    if (strcmp(media, "Recycled") == 0)      return "recycled";
    if (strcmp(media, "Glossy") == 0)        return "glossy";

    return "plain";
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return 1;     /* tray 1 (multipurpose) */
    case 4:  return 4;     /* tray 2 */
    default: return 255;   /* auto */
    }
}

/* ---------- CUPS options parsing ---------- */

static void
parse_options(const char *options, int *model, int *duplex, int *copies)
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
        if (m >= 0 && m <= 2)
            *model = m;
    }

    val = cupsGetOption("Duplex", num_opts, opts);
    if (val)
    {
        if (strcmp(val, "None") == 0 || strcmp(val, "Off") == 0)
            *duplex = 1;
        else if (strcmp(val, "DuplexNoTumble") == 0)
            *duplex = 2;
        else if (strcmp(val, "DuplexTumble") == 0)
            *duplex = 3;
    }

    val = cupsGetOption("copies", num_opts, opts);
    if (val)
    {
        int c = atoi(val);
        if (c >= 1)
            *copies = c;
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
    int                 duplex = 1;    /* 1=off, 2=longedge, 3=shortedge */
    int                 copies = 1;
    const char          *jobTitle;
    const char          *userName;

    /* CUPS filter expects: filter job user title copies options [file] */
    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    /* Set up signal handlers for clean cancellation */
    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    userName = argv[2];
    jobTitle = argv[3];

    /* Parse copies from argv[4] as baseline */
    copies = atoi(argv[4]);
    if (copies < 1)
        copies = 1;

    /* Parse options from argv[5] -- may override model, duplex, copies */
    parse_options(argv[5], &Model, &duplex, &copies);

    /* Open input */
    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertolava: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
    {
        fd = 0; /* stdin */
    }

    /* Open CUPS raster stream */
    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertolava: cannot open raster stream\n");
        if (fd != 0)
            close(fd);
        return 1;
    }

    /* We need resolution from the first page header to emit start_doc,
     * so we use a do-while style: read header first, emit doc header
     * before the first page, then loop. */
    {
        int docStarted = 0;

        while (!Canceled && cupsRasterReadHeader2(ras, &header))
        {
            unsigned int    cupsW, cupsH, cupsBpl;
            int             resX, resY;
            int             jbigW;
            int             bpl, bpl16;
            unsigned char   *buf;
            unsigned char   *bitmaps[1];
            struct jbg_enc_state se;
            BIE_CHAIN       *chain = NULL;
            int             paperCode, mediaCode, sourceCode;
            const char      *paperStr, *mediaStr;
            unsigned int    y;
            int             invert;
            int             totalDots, blackDots;

            /* Emit document header before the first page */
            if (!docStarted)
            {
                start_doc(stdout, jobTitle, userName, duplex,
                          header.HWResolution[0], header.HWResolution[1],
                          copies);
                docStarted = 1;
            }

            page++;

            /* Extract raster dimensions */
            cupsW   = header.cupsWidth;
            cupsH   = header.cupsHeight;
            cupsBpl = header.cupsBytesPerLine;
            resX    = header.HWResolution[0];
            resY    = header.HWResolution[1];

            fprintf(stderr, "DEBUG: rastertolava: page %d, %ux%u pixels, "
                    "%dx%d dpi, colorspace %d, model %d\n",
                    page, cupsW, cupsH, resX, resY,
                    header.cupsColorSpace, Model);

            /* Validate -- we only handle 1bpp monochrome */
            if (header.cupsBitsPerPixel != 1)
            {
                fprintf(stderr,
                        "ERROR: rastertolava: expected 1bpp, got %d\n",
                        header.cupsBitsPerPixel);
                break;
            }

            /* Determine if we need to invert (CUPS_CSPACE_W = white is 1) */
            invert = (header.cupsColorSpace == CUPS_CSPACE_W);

            /* Map CUPS header to LAVAFLOW codes */
            paperCode  = map_paper_code(header.cupsPageSizeName);
            paperStr   = map_paper_str(header.cupsPageSizeName);
            mediaCode  = map_media_code(header.MediaType);
            mediaStr   = map_media_str(header.MediaType);
            sourceCode = map_source_code(header.MediaPosition);

            /* Width for JBIG: use actual pixel width (no padding needed
             * for LAVAFLOW -- foo2lava uses raw width). */
            jbigW = cupsW;
            bpl   = (jbigW + 7) / 8;
            bpl16 = bpl;  /* LAVAFLOW needs no 16-byte row alignment */

            /* Allocate page buffer (zero-filled for padding) */
            buf = calloc(bpl16, cupsH);
            if (!buf)
            {
                fprintf(stderr,
                        "ERROR: rastertolava: cannot allocate page buffer "
                        "(%u bytes)\n", bpl16 * cupsH);
                break;
            }

            /* Read raster scanlines into buffer */
            for (y = 0; y < cupsH && !Canceled; y++)
            {
                unsigned int n = cupsRasterReadPixels(ras,
                        buf + (size_t)y * bpl16, cupsBpl);
                if (n == 0)
                    break;

                /* Invert if colorspace is white-is-1 */
                if (invert)
                {
                    unsigned int i;
                    unsigned char *row = buf + (size_t)y * bpl16;
                    for (i = 0; i < cupsBpl; i++)
                        row[i] ^= 0xFF;
                }
            }

            if (Canceled)
            {
                free(buf);
                break;
            }

            /* Compute dot statistics */
            totalDots = jbigW * cupsH;
            blackDots = compute_image_dots(jbigW, cupsH, buf, bpl16);

            /* JBIG encode the page */
            *bitmaps = buf;
            jbg_enc_init(&se, jbigW, cupsH, 1, bitmaps,
                         output_jbig, &chain);
            jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                            JbgOptions[2], JbgOptions[3], JbgOptions[4]);
            jbg_enc_out(&se);
            jbg_enc_free(&se);

            /* Write LAVAFLOW page */
            start_page(stdout, resX, resY,
                       (unsigned long)jbigW, (unsigned long)cupsH,
                       paperCode, paperStr,
                       sourceCode, mediaCode, mediaStr);
            write_plane(4, &chain, stdout, totalDots, blackDots);
            end_page(stdout);

            free(buf);

            /* CUPS page accounting */
            fprintf(stderr, "PAGE: %d %d\n", page,
                    header.NumCopies > 0 ? header.NumCopies : 1);
        }

        /* If no pages were found, still emit doc header with defaults */
        if (!docStarted)
            start_doc(stdout, jobTitle, userName, duplex, 1200, 600, copies);
    }
    /* Write LAVAFLOW document trailer */
    end_doc(stdout);

    /* Cleanup */
    cupsRasterClose(ras);
    if (fd != 0)
        close(fd);

    return Canceled ? 1 : 0;
}
