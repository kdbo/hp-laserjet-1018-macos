/*
 * rastertohbpl2.c -- CUPS raster filter for HBPL2 printers
 *
 * Converts CUPS raster input to HBPL2 format with JBIG compression.
 * Designed to work with macOS's built-in cgpdftoraster filter.
 *
 * Pipeline: PDF -> cgpdftoraster (macOS built-in) -> rastertohbpl2 -> printer
 *
 * Supported printers:
 *   Dell 1355cnw, Dell C1765nf/nfw
 *   Epson AcuLaser M1400, Epson AcuLaser CX17NF
 *   Fuji Xerox DocuPrint CM205/CM215/M215/P205
 *   Xerox WorkCentre 3045/6015
 *
 * Based on foo2hbpl2 from the foo2zjs project by Rick Richardson.
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
#include "hbpl.h"

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

static int
size_chain(BIE_CHAIN *chain)
{
    int size = 0;
    while (chain)
    {
        if (chain->data)
            size += chain->len;
        chain = chain->next;
    }
    return size;
}

/*
 * JBIG encoding parameters -- must match what the printer expects.
 * HBPL2 printers use: ILEAVE|SMID, DELAY_AT|LRLTWO|TPDON|TPBON|DPON,
 * L0=128, MX=16, MY=0.
 */
static long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,                                         /* Order */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON, /* Options */
    128,                                                            /* L0 */
    16,                                                             /* MX */
    0                                                               /* MY */
};

/*
 * JBIG output callback -- builds a linked list of compressed data.
 * First item is always the 20-byte BIH. Subsequent items are 64KB max.
 */
static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 65536;

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertohbpl2: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohbpl2: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohbpl2: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- HBPL2 write_plane ---------- */

/*
 * Write all JBIG chain data (BIH + BID blocks) concatenated.
 * HBPL2 uses no framing around the JBIG data itself.
 */
static int
write_plane(int planeNum, BIE_CHAIN **root, FILE *fp)
{
    BIE_CHAIN *current = *root;

    (void)planeNum;

    if (!current)
    {
        fprintf(stderr, "ERROR: rastertohbpl2: no JBIG data\n");
        return 1;
    }
    if (!current->next)
    {
        fprintf(stderr, "ERROR: rastertohbpl2: no JBIG header\n");
        return 1;
    }
    if (current->len != 20)
    {
        fprintf(stderr, "ERROR: rastertohbpl2: wrong BIH length\n");
        return 1;
    }

    for (current = *root; current && current->len; current = current->next)
    {
        if (fwrite(current->data, 1, current->len, fp) != current->len)
        {
            fprintf(stderr, "ERROR: rastertohbpl2: write JBIG data failed\n");
            return 1;
        }
    }

    free_chain(*root);
    *root = NULL;
    return 0;
}

/* ---------- HBPL2 document/page output ---------- */

static void
start_doc(FILE *fp, const char *user, const char *title, int copies, int bpp)
{
    HBPL_JP jp;

    /* PJL header */
    fprintf(fp, "\033%%-12345X@PJL JOB NAME=PRINTER\r\n");
    fprintf(fp, "@PJL SET JOBATTR=\"USER:%s\"\r\n", user ? user : "");
    fprintf(fp, "@PJL SET JOBATTR=\"DOCU:%s\"\r\n", title ? title : "");
    fprintf(fp, "@PJL SET JOBATTR=\"OWNR:%s\"\r\n", user ? user : "");
    fprintf(fp, "@PJL SET DUPLEX=OFF\r\n");
    fprintf(fp, "@PJL SET IWAMANUALDUP=OFF\r\n");
    fprintf(fp, "@PJL SET MEDIASOURCE=0\r\n");
    fprintf(fp, "@PJL SET RENDERMODE=GRAYSCALE\r\n");
    fprintf(fp, "@PJL SET RESOLUTION=600\r\n");
    fprintf(fp, "@PJL SET BITSPERPIXEL=%d\r\n", bpp);
    fprintf(fp, "@PJL SET COPIES=%d\r\n", copies);
    fprintf(fp, "@PJL ENTER LANGUAGE=HBPL\r\n");

    /* HBPL_JP binary struct */
    memset(&jp, 0, sizeof(jp));
    jp.hdr.type[0] = '\033';
    jp.hdr.type[1] = 'J';
    jp.hdr.type[2] = 'P';
    jp.hdr.len = (char)(sizeof(HBPL_JP) - 4);
    jp.unk1 = le32(0x01000001);
    fwrite(&jp, 1, sizeof(jp), fp);
}

static void
end_doc(FILE *fp)
{
    fprintf(fp, "\033%%-12345X@PJL EOJ\r\n");
}

/*
 * Write the HBPL_DM struct every 4 pages (when pageNum % 4 == 0).
 */
static void
write_dm(FILE *fp)
{
    HBPL_DM dm;

    memset(&dm, 0, sizeof(dm));
    dm.hdr.type[0] = '\033';
    dm.hdr.type[1] = 'D';
    dm.hdr.type[2] = 'M';
    dm.hdr.len = (char)(sizeof(HBPL_DM) - 4);
    dm.data = 0;
    fwrite(&dm, 1, sizeof(dm), fp);
}

/*
 * Write the HBPL_PS (page start) struct.
 * Must be called AFTER JBIG encoding so that the total chain size is known.
 */
static void
start_page(FILE *fp, unsigned long jbigW, unsigned long height,
           int bpp, int chainSize,
           int paperCode, int mediaCode, int resY)
{
    HBPL_PS ps;

    memset(&ps, 0, sizeof(ps));
    ps.hdr.type[0] = '\033';
    ps.hdr.type[1] = 'P';
    ps.hdr.type[2] = 'S';
    ps.hdr.len = (char)(sizeof(HBPL_PS) - 4);
    ps.w = le32(jbigW / bpp);
    ps.h = le32(height);
    ps.wh_total = le32((jbigW * height) / 8);  /* mono: total bits / 8 */
    ps.len = le32(chainSize);
    ps.papersize = (char)paperCode;
    ps.mediatype = (char)mediaCode;
    ps.color = 0;       /* mono */
    ps.unk2 = 0;
    ps.res = le16((uint16_t)resY);
    ps.bihoff[0] = le32(0);
    ps.bihoff[1] = le32(0);
    ps.bihoff[2] = le32(0);
    ps.bihoff[3] = le32(chainSize);
    fwrite(&ps, 1, sizeof(ps), fp);
}

static void
end_page(FILE *fp)
{
    HBPL_PE pe;

    memset(&pe, 0, sizeof(pe));
    pe.hdr.type[0] = '\033';
    pe.hdr.type[1] = 'P';
    pe.hdr.type[2] = 'E';
    pe.hdr.len = (char)(sizeof(HBPL_PE) - 4);
    fwrite(&pe, 1, sizeof(pe), fp);
}

/* ---------- Mapping functions ---------- */

/*
 * HBPL2 paper codes (from PPD, different from Windows DMPAPER codes):
 *   4=letter, 5=legal, 7=executive, 9=A4, 11=A5, 13=B5,
 *   20=env#10, 27=envDL, 28=envC5, 37=envMonarch
 */
static int
map_paper_code(const char *name)
{
    if (!name || !name[0])
        return 4; /* letter */

    if (strcmp(name, "Letter") == 0)       return 4;
    if (strcmp(name, "Legal") == 0)        return 5;
    if (strcmp(name, "Executive") == 0)    return 7;
    if (strcmp(name, "A4") == 0)           return 9;
    if (strcmp(name, "A5") == 0)           return 11;
    if (strcmp(name, "B5") == 0)           return 13;
    if (strcmp(name, "Env10") == 0)        return 20;
    if (strcmp(name, "EnvDL") == 0)        return 27;
    if (strcmp(name, "EnvC5") == 0)        return 28;
    if (strcmp(name, "EnvMonarch") == 0)   return 37;

    return 4; /* default: letter */
}

/*
 * HBPL2 media codes (from zjs.h):
 *   1=standard, 2=transparency, 0x101=envelope, 0x103=letterhead,
 *   0x105=thick, 0x107=labels
 */
static int
map_media_code(const char *media)
{
    if (!media || !media[0])
        return DMMEDIA_STANDARD;

    if (strcmp(media, "Plain") == 0)         return DMMEDIA_STANDARD;
    if (strcmp(media, "Transparency") == 0)  return DMMEDIA_TRANSPARENCY;
    if (strcmp(media, "Envelope") == 0)      return DMMEDIA_ENVELOPE;
    if (strcmp(media, "Letterhead") == 0)    return DMMEDIA_LETTERHEAD;
    if (strcmp(media, "Heavy") == 0)         return DMMEDIA_THICK_STOCK;
    if (strcmp(media, "Labels") == 0)        return DMMEDIA_LABELS;

    return DMMEDIA_STANDARD;
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
    int                 pageNum = 0;
    int                 copies = 1;
    const char          *user = NULL;
    const char          *title = NULL;

    /* CUPS filter expects: filter job user title copies options [file] */
    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    /* Extract CUPS filter arguments */
    user = argv[2];
    title = argv[3];
    copies = atoi(argv[4]);
    if (copies < 1)
        copies = 1;

    /* Set up signal handlers for clean cancellation */
    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Open input */
    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertohbpl2: cannot open %s\n", argv[6]);
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
        fprintf(stderr, "ERROR: rastertohbpl2: cannot open raster stream\n");
        if (fd != 0)
            close(fd);
        return 1;
    }

    /* Write HBPL2 document header.
     * Bpp is derived from the first page's resolution. For the PJL header
     * we use 1 as default; all supported printers are 1bpp mono. */
    start_doc(stdout, user, title, copies, 1);

    /* Process each page */
    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY, bpp;
        int             jbigW;
        int             bpl, bpl16;
        unsigned char   *buf;
        unsigned char   *bitmaps[1];
        struct jbg_enc_state se;
        BIE_CHAIN       *chain = NULL;
        int             paperCode, mediaCode;
        int             chainSize;
        unsigned int    y;
        int             invert;

        pageNum++;

        /* Extract raster dimensions */
        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];

        fprintf(stderr, "DEBUG: rastertohbpl2: page %d, %ux%u pixels, "
                "%dx%d dpi, colorspace %d\n",
                pageNum, cupsW, cupsH, resX, resY, header.cupsColorSpace);

        /* Validate -- we only handle 1bpp monochrome */
        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertohbpl2: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        /* Determine if we need to invert (CUPS_CSPACE_W = white is 1) */
        invert = (header.cupsColorSpace == CUPS_CSPACE_W);

        /* Bits per pixel multiplier from resolution */
        bpp = resX / 600;
        if (bpp < 1) bpp = 1;

        /* Map CUPS header to HBPL2 codes */
        paperCode = map_paper_code(header.cupsPageSizeName);
        mediaCode = map_media_code(header.MediaType);

        /* Width padding for JBIG: round up to 128-pixel boundary */
        jbigW = (cupsW + 127) & ~127;
        bpl   = (jbigW + 7) / 8;
        bpl16 = (bpl + 15) & ~15;

        /* Allocate page buffer (zero-filled for padding) */
        buf = calloc(bpl16, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertohbpl2: cannot allocate page buffer "
                    "(%u bytes)\n", bpl16 * cupsH);
            break;
        }

        /* Read raster scanlines into padded buffer */
        for (y = 0; y < cupsH && !Canceled; y++)
        {
            unsigned int n = cupsRasterReadPixels(ras,
                                                  buf + (size_t)y * bpl16,
                                                  cupsBpl);
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

        /* JBIG encode the full page */
        *bitmaps = buf;
        jbg_enc_init(&se, jbigW, cupsH, 1, bitmaps, output_jbig, &chain);
        jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                        JbgOptions[2], JbgOptions[3], JbgOptions[4]);
        jbg_enc_out(&se);
        jbg_enc_free(&se);

        /* Calculate total JBIG chain size (needed before writing HBPL_PS) */
        chainSize = size_chain(chain);

        /* Write HBPL_DM every 4 pages */
        if ((pageNum % 4) == 1)
            write_dm(stdout);

        /* Write HBPL_PS (page start) with known JBIG size */
        start_page(stdout, jbigW, cupsH, bpp, chainSize,
                   paperCode, mediaCode, resY);

        /* Write JBIG data (BIH + BID blocks, no framing) */
        write_plane(4, &chain, stdout);

        /* Write HBPL_PE (page end) */
        end_page(stdout);

        free(buf);

        /* CUPS page accounting */
        fprintf(stderr, "PAGE: %d %d\n", pageNum,
                header.NumCopies > 0 ? header.NumCopies : 1);
    }

    /* Write document trailer */
    end_doc(stdout);

    /* Cleanup */
    cupsRasterClose(ras);
    if (fd != 0)
        close(fd);

    return Canceled ? 1 : 0;
}
