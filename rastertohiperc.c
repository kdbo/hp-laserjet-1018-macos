/*
 * rastertohiperc.c — CUPS raster filter for Oki HiPerC printers
 *
 * Converts CUPS raster input to Oki HiPerC format with JBIG compression.
 *
 * Supported printers:
 *   Oki C110, C301dn, C310dn, C3100, C3200, C3300, C3400, C3530 MFP,
 *   C5100, C511dn, C5200, C5500, C5600, C5650, C5800, C810
 *
 * Pipeline: PDF → cgpdftoraster (macOS built-in) → rastertohiperc → printer
 *
 * Based on foo2hiperc from the foo2zjs project by Rick Richardson.
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
#include "hiperc.h"

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

/* HiPerC JBIG options — different from XQX/ZJS */
static long JbgOptions[5] = {
    0,                                          /* Order (no ILEAVE/SMID) */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPBON,     /* Options (no DPON) */
    256,                                        /* L0 (256, not 128) */
    16,                                         /* MX */
    0                                           /* MY */
};

static void
output_jbig(unsigned char *start, size_t len, void *cbarg)
{
    BIE_CHAIN *current, **root = (BIE_CHAIN **)cbarg;
    int size = 0x80000; /* HiPerC uses 512KB chunks */

    if (*root == NULL)
    {
        *root = malloc(sizeof(BIE_CHAIN));
        if (!*root)
        {
            fprintf(stderr, "ERROR: rastertohiperc: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohiperc: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertohiperc: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- HiPerC binary record output ---------- */

static void
start_page_record(int nbie, int w, int h, int plane, int resX, int resY,
                  unsigned char *bih, FILE *fp)
{
    int h256 = ((h + 255) / 256) * 256;
    DWORD rec[13];
    int resmode;

    /* Resolution mode byte */
    if (resX == 300)
        resmode = 0;
    else if (resY == 1200)
        resmode = 33;
    else
        resmode = 17; /* 600x600 */

    rec[0] = be32(52);                                          /* reclen */
    rec[1] = be32(HIPERC_START_PAGE);                           /* rectype=0 */

    rec[2] = be32(16);                                          /* block0: len */
    rec[3] = be32((nbie << 24) + (plane << 16) + (128 << 8) + resmode);
    rec[4] = be32(w);                                           /* block0: width */
    rec[5] = be32(0);
    rec[6] = be32(0x10000000);

    rec[7] = be32(20);                                          /* block1: len */
    fwrite(rec, 32, 1, fp);

    /* Write JBIG BIH with height padded to 256 */
    ((DWORD *)bih)[2] = be32(h256);
    fwrite(bih, 20, 1, fp);
}

static void
write_data_record(int plane, unsigned char *data, int len, FILE *fp)
{
    DWORD rec[5];

    rec[0] = be32(len + 20);                   /* reclen */
    rec[1] = be32(HIPERC_DATA);                /* rectype=1 */
    rec[2] = be32(4);                          /* block0: len */
    rec[3] = be32(plane << 24);                /* block0: plane */
    rec[4] = be32(len);                        /* block1: data len */
    fwrite(rec, 20, 1, fp);
    fwrite(data, 1, len, fp);
}

static int
write_plane(int planeNum, BIE_CHAIN **root, FILE *fp,
            int resX, int resY)
{
    BIE_CHAIN *current = *root;
    int plane = planeNum - 1; /* 0-based for HiPerC */

    if (!current || !current->next || current->len != 20)
    {
        fprintf(stderr, "ERROR: rastertohiperc: invalid JBIG data\n");
        return 1;
    }

    /* First chunk is BIH — extract dimensions and write start_page */
    int w = (((long)current->data[4] << 24) |
             ((long)current->data[5] << 16) |
             ((long)current->data[6] << 8) |
             (long)current->data[7]);
    int h = (((long)current->data[8] << 24) |
             ((long)current->data[9] << 16) |
             ((long)current->data[10] << 8) |
             (long)current->data[11]);

    start_page_record(1, w, h, plane, resX, resY, current->data, fp);

    /* Write each JBIG data block as a DATA record */
    for (current = (*root)->next; current && current->len;
         current = current->next)
    {
        write_data_record(plane, current->data, current->len, fp);
    }

    free_chain(*root);
    return 0;
}

static void
end_page(FILE *fp)
{
    DWORD rec[2];
    rec[0] = be32(8);
    rec[1] = be32(HIPERC_END_PAGE); /* 255 */
    fwrite(rec, 8, 1, fp);
}

/* ---------- PJL preamble/postamble (Oki-specific) ---------- */

static void
start_doc(FILE *fp, int paperCode, int mediaCode, int sourceCode,
          int copies, int resX, int resY, int pageW, int pageH)
{
    time_t now;
    struct tm *tmp;
    char datetime[256 + 1];

    static const char *strmedia[] = {
        "PLAIN", "THICK", "THIN", "BOND", "COLOR",
        "CARDSTOCK", "LABELS", "ENVELOPE", "PREPRINTED",
        "COTTON", "RECYCLED"
    };
    static const char *strpaper[] = {
        "CUSTOM", "A4", "LETTER", "LEGAL", "LEGAL13",
        "A5", "B5", "A6", "MONARCH", "DL",
        "C5", "COM10", "EXECUTIVE", "COM9", "LEGAL135",
        "A3", "TABLOID"
    };

    #define STRARY(X, A) \
        ((X) >= 0 && (X) < (int)(sizeof(A)/sizeof(A[0]))) ? A[X] : "NORMAL"

    fprintf(fp, "\033%%-12345X@PJL\r\n");
    fprintf(fp, "@PJL RDYMSG DISPLAY = \"Ready\"\r\n");
    fprintf(fp, "@PJL SET OKIJOBACCOUNTJOB USERID=\"Unknown\" JOBNAME=\"Unknown\"\r\n");
    fprintf(fp, "@PJL SET OKIAUXJOBINFO DATA=\"DocumentName=Unknown\"\r\n");

    now = time(NULL);
    tmp = localtime(&now);
    strftime(datetime, sizeof(datetime), "00:00:00 %Y/%m/%d", tmp);
    fprintf(fp, "@PJL SET OKIAUXJOBINFO DATA=\"ReceptionTime=%s\"\r\n", datetime);

    if (sourceCode == DMBIN_AUTO)
        fprintf(fp, "@PJL SET OKIAUTOTRAYSWITCH=ON\r\n");
    else
        fprintf(fp, "@PJL SET OKIAUTOTRAYSWITCH=OFF\r\n");

    fprintf(fp, "@PJL SET OKIPAPERSIZECHECK=ENABLE\r\n");

    if (resX == 300)
        fprintf(fp, "@PJL SET RESOLUTION=300\r\n");
    else
        fprintf(fp, "@PJL SET RESOLUTION=600\r\n");

    if (resY == 1200)
        fprintf(fp, "@PJL SET RESOLUTION=V1200\r\n");

    fprintf(fp, "@PJL SET PAPER=%s\r\n", STRARY(paperCode, strpaper));
    fprintf(fp, "@PJL SET OKITRAYSEQUENCE=PAPERFEEDTRAY\r\n");

    switch (sourceCode)
    {
    case DMBIN_AUTO:
    case DMBIN_TRAY1:
        fprintf(fp, "@PJL SET OKIPAPERFEED=TRAY1\r\n");
        break;
    case DMBIN_TRAY2:
        fprintf(fp, "@PJL SET OKIPAPERFEED=TRAY2\r\n");
        break;
    case DMBIN_MULTI:
        fprintf(fp, "@PJL SET OKIPAPERFEED=FRONTTRAY\r\n");
        break;
    case DMBIN_MANUAL:
        fprintf(fp, "@PJL SET OKIPAPERFEED=FRONTTRAY\r\n");
        fprintf(fp, "@PJL SET MANUALFEED=ON\r\n");
        break;
    }

    fprintf(fp, "@PJL SET OKIMEDIATYPE = %s\r\n", STRARY(mediaCode, strmedia));
    fprintf(fp, "@PJL SET LPARM:PCL OKIPRINTMARGIN=INCH1D6\r\n");
    fprintf(fp, "@PJL SET COPIES=%d\r\n", copies);
    fprintf(fp, "@PJL SET QTY=1\r\n");
    fprintf(fp, "@PJL SET HIPERCEFFECTIVEBLOCKSIZE=%d\r\n", pageW * pageH / 8);
    fprintf(fp, "@PJL ENTER LANGUAGE=HIPERC\n");
}

static void
end_doc(FILE *fp)
{
    fprintf(fp, "\033%%-12345X@PJL\r\n");
    fprintf(fp, "@PJL EOJ NAME = \"End \"\n");
    fprintf(fp, "\033%%-12345X");
}

/* ---------- Mapping functions ---------- */

/*
 * Oki uses its own paper codes, not DMPAPER:
 * 0=custom, 1=A4, 2=letter, 3=legal, 5=A5, 6=B5, 7=A6,
 * 8=envMonarch, 9=envDL, 10=envC5, 11=env#10, 12=executive,
 * 13=env#9, 15=A3, 16=tabloid
 */
static int
map_paper_code(const char *name)
{
    if (!name || !name[0])              return 2; /* letter */

    if (strcmp(name, "Letter") == 0)     return 2;
    if (strcmp(name, "Legal") == 0)      return 3;
    if (strcmp(name, "Executive") == 0)  return 12;
    if (strcmp(name, "A3") == 0)         return 15;
    if (strcmp(name, "A4") == 0)         return 1;
    if (strcmp(name, "A5") == 0)         return 5;
    if (strcmp(name, "A6") == 0)         return 7;
    if (strcmp(name, "B5") == 0)         return 6;
    if (strcmp(name, "Tabloid") == 0)    return 16;
    if (strcmp(name, "Env10") == 0)      return 11;
    if (strcmp(name, "EnvDL") == 0)      return 9;
    if (strcmp(name, "EnvC5") == 0)      return 10;
    if (strcmp(name, "EnvMonarch") == 0) return 8;
    if (strcmp(name, "Env9") == 0)       return 13;

    return 2;
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
    if (strcmp(media, "Transparency") == 0)  return 2; /* same as THIN */

    return DMMEDIA_PLAIN;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return DMBIN_TRAY1;
    case 2:  return DMBIN_TRAY2;
    case 3:  return DMBIN_MULTI;
    case 4:  return DMBIN_MANUAL;
    default: return DMBIN_AUTO;
    }
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
    int                 first_page = 1;
    int                 paperCode = 2, mediaCode = 0, sourceCode = 0;

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n",
                argv[0]);
        return 1;
    }

    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertohiperc: cannot open %s\n", argv[6]);
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
        fprintf(stderr, "ERROR: rastertohiperc: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY;
        int             jbigW;
        int             bpl;
        unsigned char   *buf;
        unsigned char   *bitmaps[1];
        struct jbg_enc_state se;
        BIE_CHAIN       *chain = NULL;
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

        fprintf(stderr, "DEBUG: rastertohiperc: page %d, %ux%u pixels, %dx%d dpi\n",
                page, cupsW, cupsH, resX, resY);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertohiperc: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType);
        sourceCode = map_source_code(header.MediaPosition);

        /* Write PJL preamble on first page */
        if (first_page)
        {
            start_doc(stdout, paperCode, mediaCode, sourceCode,
                      copies, resX, resY, cupsW, cupsH);
            first_page = 0;
        }

        /* JBIG needs width as-is for HiPerC (no 128-pixel padding needed
         * since L0=256 and the BIH carries the actual dimensions) */
        jbigW = cupsW;
        bpl = (jbigW + 7) / 8;

        buf = calloc(bpl, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertohiperc: cannot allocate page buffer\n");
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
        jbg_enc_init(&se, jbigW, cupsH, 1, bitmaps, output_jbig, &chain);
        jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                        JbgOptions[2], JbgOptions[3], JbgOptions[4]);
        jbg_enc_out(&se);
        jbg_enc_free(&se);

        write_plane(4, &chain, stdout, resX, resY); /* plane 4=K mono */
        end_page(stdout);

        free(buf);

        fprintf(stderr, "PAGE: %d %d\n", page, copies);
    }

    end_doc(stdout);

    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return Canceled ? 1 : 0;
}
