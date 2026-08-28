/*
 * rastertooak.c -- CUPS raster filter for Oak Technologies JBIG printers
 *
 * Converts CUPS raster input to Oak Technologies OAKT format with JBIG
 * compression. The OAKT format uses little-endian JBIG BIH (unlike all
 * other foo2zjs formats which are big-endian).
 *
 * Pipeline: PDF -> cgpdftoraster (macOS built-in) -> rastertooak -> printer
 *
 * Printers: HP Color LaserJet 1500, Kyocera KM-1635, KM-2035
 *
 * Based on foo2oak from the foo2zjs project by Rick Richardson.
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
#include "oak.h"

/* ---------- Bit-mirror table (HP 1500 needs mirrored bits) ---------- */

static unsigned char Mirror1[256] =
{
      0,128, 64,192, 32,160, 96,224, 16,144, 80,208, 48,176,112,240,
      8,136, 72,200, 40,168,104,232, 24,152, 88,216, 56,184,120,248,
      4,132, 68,196, 36,164,100,228, 20,148, 84,212, 52,180,116,244,
     12,140, 76,204, 44,172,108,236, 28,156, 92,220, 60,188,124,252,
      2,130, 66,194, 34,162, 98,226, 18,146, 82,210, 50,178,114,242,
     10,138, 74,202, 42,170,106,234, 26,154, 90,218, 58,186,122,250,
      6,134, 70,198, 38,166,102,230, 22,150, 86,214, 54,182,118,246,
     14,142, 78,206, 46,174,110,238, 30,158, 94,222, 62,190,126,254,
      1,129, 65,193, 33,161, 97,225, 17,145, 81,209, 49,177,113,241,
      9,137, 73,201, 41,169,105,233, 25,153, 89,217, 57,185,121,249,
      5,133, 69,197, 37,165,101,229, 21,149, 85,213, 53,181,117,245,
     13,141, 77,205, 45,173,109,237, 29,157, 93,221, 61,189,125,253,
      3,131, 67,195, 35,163, 99,227, 19,147, 83,211, 51,179,115,243,
     11,139, 75,203, 43,171,107,235, 27,155, 91,219, 59,187,123,251,
      7,135, 71,199, 39,167,103,231, 23,151, 87,215, 55,183,119,247,
     15,143, 79,207, 47,175,111,239, 31,159, 95,223, 63,191,127,255,
};

static void
mirror_bytes(unsigned char *sp, int bpl, unsigned char *mirror)
{
    unsigned char *ep = sp + bpl - 1;
    unsigned char tmp;

    while (sp < ep)
    {
        tmp = mirror[*sp];
        *sp = mirror[*ep];
        *ep = tmp;
        ++sp;
        --ep;
    }
    if (sp == ep)
        *sp = mirror[*sp];
}

/* ---------- Oak record writer ---------- */

static int
oak_record(FILE *fp, int type, void *payload, int paylen)
{
    OAK_HDR hdr;
    static char pad[] = "PAD_PAD_PAD_PAD_";

    memcpy(hdr.magic, OAK_HDR_MAGIC, sizeof(hdr.magic));
    hdr.type = type;
    hdr.len = (sizeof(hdr) + paylen + 15) & ~0x0f;

    if (fwrite(&hdr, 1, sizeof(hdr), fp) == 0)
    {
        fprintf(stderr, "ERROR: rastertooak: oak_record write failed\n");
        exit(1);
    }
    if (payload && paylen)
    {
        if (fwrite(payload, 1, paylen, fp) == 0)
        {
            fprintf(stderr, "ERROR: rastertooak: oak_record payload failed\n");
            exit(1);
        }
    }
    if (hdr.len - (sizeof(hdr) + paylen))
    {
        if (fwrite(pad, 1, hdr.len - (sizeof(hdr) + paylen), fp) == 0)
        {
            fprintf(stderr, "ERROR: rastertooak: oak_record pad failed\n");
            exit(1);
        }
    }

    return 0;
}

/* ---------- Byte-swap helper for little-endian BIH ---------- */

static void
iswap32(void *p)
{
    char *cp = (char *)p;
    char tmp;
    tmp = cp[0]; cp[0] = cp[3]; cp[3] = tmp;
    tmp = cp[1]; cp[1] = cp[2]; cp[2] = tmp;
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

/* JBIG encoding parameters -- match foo2oak defaults */
static long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,                                     /* Order */
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON,        /* Options */
    128,                                                        /* L0 (dynamic) */
    16,                                                         /* MX */
    0                                                           /* MY */
};

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
            fprintf(stderr, "ERROR: rastertooak: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertooak: malloc failed\n");
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
                fprintf(stderr, "ERROR: rastertooak: malloc failed\n");
                exit(1);
            }
            current = current->next;
            current->data = NULL;
            current->next = NULL;
            current->len = 0;
        }
    }
}

/* ---------- OAK image plane helper ---------- */

static void
fill_image_plane_unknown(OAK_IMAGE_PLANE *ip)
{
    int i;
    for (i = 0; i < 16; ++i)
        ip->unk[i] = i;
}

/* ---------- Document start/end ---------- */

static void
start_doc(FILE *fp)
{
    OAK_OTHER       recother;
    OAK_TIME        rectime;
    OAK_FILENAME    recfile;
    OAK_DUPLEX      recduplex;
    time_t          now;
    struct tm       *tm;

    memset(&recother, 0, sizeof(recother));
    recother.unk = 1;
    strcpy(recother.string, "OTHER");
    oak_record(fp, OAK_TYPE_OTHER, &recother, sizeof(recother));

    memset(&rectime, 0, sizeof(rectime));
    time(&now);
    strcpy(rectime.datetime, ctime(&now));
    rectime.time_t = now;
    tm = localtime(&now);
    rectime.year = tm->tm_year + 1900;
    rectime.tm_mon = tm->tm_mon;
    rectime.tm_mday = tm->tm_mday;
    rectime.tm_hour = tm->tm_hour;
    rectime.tm_min = tm->tm_min;
    rectime.tm_sec = tm->tm_sec;
    oak_record(fp, OAK_TYPE_TIME, &rectime, sizeof(rectime));

    memset(&recfile, 0, sizeof(recfile));
    strcpy(recfile.string, "stdin");
    oak_record(fp, OAK_TYPE_FILENAME, &recfile, sizeof(recfile));

    memset(&recduplex, 0, sizeof(recduplex));
    recduplex.duplex = 0;
    recduplex.short_edge = 0;
    oak_record(fp, OAK_TYPE_DUPLEX, &recduplex, sizeof(recduplex));
}

static void
end_doc(FILE *fp)
{
    oak_record(fp, OAK_TYPE_END_DOC, NULL, 0);
}

/* ---------- Mapping functions ---------- */

static int
map_paper_code(const char *name)
{
    if (!name || !name[0])           return OAK_PAPER_LETTER;
    if (strcmp(name, "Letter") == 0)  return OAK_PAPER_LETTER;
    if (strcmp(name, "Legal") == 0)   return OAK_PAPER_LEGAL;
    if (strcmp(name, "Executive") == 0) return OAK_PAPER_EXECUTIVE;
    if (strcmp(name, "A4") == 0)     return OAK_PAPER_A4;
    if (strcmp(name, "A5") == 0)     return OAK_PAPER_A5;
    if (strcmp(name, "B5") == 0)     return OAK_PAPER_B5_JIS;
    if (strcmp(name, "Env10") == 0)  return OAK_PAPER_ENV_10;
    if (strcmp(name, "EnvDL") == 0)  return OAK_PAPER_ENV_DL;
    if (strcmp(name, "EnvC5") == 0)  return OAK_PAPER_ENV_C5;
    if (strcmp(name, "EnvMonarch") == 0) return OAK_PAPER_ENV_MONARCH;
    return OAK_PAPER_LETTER;
}

static int
map_media_code(const char *media)
{
    if (!media || !media[0])            return OAK_MEDIA_AUTO;
    if (strcmp(media, "Plain") == 0)     return OAK_MEDIA_PLAIN;
    if (strcmp(media, "Transparency") == 0) return OAK_MEDIA_GRAYTRANS;
    if (strcmp(media, "Envelope") == 0)  return OAK_MEDIA_ENVELOPE;
    if (strcmp(media, "Labels") == 0)    return OAK_MEDIA_LABELS;
    if (strcmp(media, "Heavy") == 0)     return OAK_MEDIA_HEAVY;
    if (strcmp(media, "Letterhead") == 0) return OAK_MEDIA_LETTERHEAD;
    if (strcmp(media, "Bond") == 0)      return OAK_MEDIA_BOND;
    if (strcmp(media, "Recycled") == 0)  return OAK_MEDIA_RECYCLED;
    if (strcmp(media, "Cardstock") == 0) return OAK_MEDIA_CARDSTOCK;
    return OAK_MEDIA_AUTO;
}

static int
map_source_code(unsigned media_position)
{
    switch (media_position)
    {
    case 1:  return OAK_SOURCE_TRAY1;
    case 2:  return OAK_SOURCE_TRAY2;
    case 4:  return OAK_SOURCE_MANUAL;
    default: return OAK_SOURCE_AUTO;
    }
}

/* ---------- CUPS options parsing ---------- */

static void
parse_options(const char *options, int *mirror)
{
    cups_option_t *opts = NULL;
    int num_opts;
    const char *val;

    if (!options || !options[0])
        return;

    num_opts = cupsParseOptions(options, 0, &opts);

    /* Mirror defaults to 1 (HP 1500 style). Set to 0 for KM-1635/2035. */
    val = cupsGetOption("Mirror", num_opts, opts);
    if (val)
        *mirror = atoi(val);

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

#define STRIPE_HEIGHT 256

int
main(int argc, char *argv[])
{
    cups_raster_t       *ras;
    cups_page_header2_t header;
    int                 fd;
    int                 page = 0;
    int                 mirror = 1;  /* HP 1500 default */

    if (argc < 6 || argc > 7)
    {
        fprintf(stderr, "Usage: %s job user title copies options [file]\n", argv[0]);
        return 1;
    }

    signal(SIGTERM, cancel_handler);
    signal(SIGPIPE, SIG_IGN);

    parse_options(argv[5], &mirror);

    if (argc == 7)
    {
        fd = open(argv[6], O_RDONLY);
        if (fd < 0)
        {
            fprintf(stderr, "ERROR: rastertooak: cannot open %s\n", argv[6]);
            return 1;
        }
    }
    else
        fd = 0;

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras)
    {
        fprintf(stderr, "ERROR: rastertooak: cannot open raster stream\n");
        if (fd != 0) close(fd);
        return 1;
    }

    start_doc(stdout);

    while (!Canceled && cupsRasterReadHeader2(ras, &header))
    {
        unsigned int    cupsW, cupsH, cupsBpl;
        int             resX, resY;
        int             bpl;
        unsigned char   *buf;
        unsigned int    y;
        int             invert;
        int             paperCode, mediaCode, sourceCode, copies;
        DWORD           source_arg;
        OAK_MEDIA       recmedia;
        OAK_COPIES      reccopies;
        OAK_PAPER       recpaper;
        OAK_IMAGE_MONO  recmono;
        WORD            endpage_arg;

        page++;
        cupsW   = header.cupsWidth;
        cupsH   = header.cupsHeight;
        cupsBpl = header.cupsBytesPerLine;
        resX    = header.HWResolution[0];
        resY    = header.HWResolution[1];
        copies  = header.NumCopies > 0 ? header.NumCopies : 1;

        fprintf(stderr, "DEBUG: rastertooak: page %d, %ux%u pixels, %dx%d dpi\n",
                page, cupsW, cupsH, resX, resY);

        if (header.cupsBitsPerPixel != 1)
        {
            fprintf(stderr, "ERROR: rastertooak: expected 1bpp, got %d\n",
                    header.cupsBitsPerPixel);
            break;
        }

        invert = (header.cupsColorSpace == CUPS_CSPACE_W);
        bpl = (cupsW + 7) / 8;

        paperCode  = map_paper_code(header.cupsPageSizeName);
        mediaCode  = map_media_code(header.MediaType);
        sourceCode = map_source_code(header.MediaPosition);

        /* Allocate page buffer */
        buf = calloc(bpl, cupsH);
        if (!buf)
        {
            fprintf(stderr, "ERROR: rastertooak: malloc failed\n");
            break;
        }

        /* Read raster scanlines */
        for (y = 0; y < cupsH && !Canceled; y++)
        {
            unsigned int n = cupsRasterReadPixels(ras,
                                buf + (size_t)y * bpl, cupsBpl);
            if (n == 0) break;
            if (invert)
            {
                unsigned int idx;
                unsigned char *row = buf + (size_t)y * bpl;
                for (idx = 0; idx < cupsBpl; idx++)
                    row[idx] ^= 0xFF;
            }
        }

        if (Canceled) { free(buf); break; }

        /* Mirror bits for HP 1500 */
        if (mirror)
        {
            for (y = 0; y < cupsH; ++y)
                mirror_bytes(buf + y * bpl, bpl, Mirror1);
        }

        /* START_PAGE */
        oak_record(stdout, OAK_TYPE_START_PAGE, NULL, 0);

        /* Page parameters */
        source_arg = sourceCode;
        oak_record(stdout, OAK_TYPE_SOURCE, &source_arg, sizeof(source_arg));

        memset(&recmedia, 0, sizeof(recmedia));
        recmedia.media = mediaCode;
        recmedia.unk8[0] = 2;
        recmedia.unk8[1] = 0;
        recmedia.unk8[2] = 0;
        memset(recmedia.string, ' ', sizeof(recmedia.string));
        recmedia.string[0] = '\0';
        oak_record(stdout, OAK_TYPE_MEDIA, &recmedia, sizeof(recmedia));

        reccopies.copies = copies;
        reccopies.duplex = 0;  /* no duplex */
        oak_record(stdout, OAK_TYPE_COPIES, &reccopies, sizeof(reccopies));

        recpaper.paper = paperCode;
        recpaper.w1200 = cupsW * 1200 / resX;
        recpaper.h1200 = cupsH * 1200 / resY;
        recpaper.unk = 0;
        oak_record(stdout, OAK_TYPE_PAPER, &recpaper, sizeof(recpaper));

        /* Image header (mono) */
        recmono.plane.unk0 = 0;
        recmono.plane.unk1 = 1;     /* mono: 1 */
        recmono.plane.w = cupsW;
        recmono.plane.h = cupsH;
        recmono.plane.resx = resX;
        recmono.plane.resy = resY;
        recmono.plane.nbits = 1;
        fill_image_plane_unknown(&recmono.plane);
        oak_record(stdout, OAK_TYPE_IMAGE_MONO, &recmono, sizeof(recmono));

        oak_record(stdout, OAK_TYPE_START_IMAGE, NULL, 0);

        /* Output image stripes */
        for (y = 0; y < cupsH; y += STRIPE_HEIGHT)
        {
            struct jbg_enc_state se;
            unsigned char       *bitmaps[1];
            BIE_CHAIN           *chain = NULL;
            BIE_CHAIN           *current;
            OAK_IMAGE_DATA      recdata;
            int                 chainlen;
            int                 padlen;
            static char         pad[] = "PAD_PAD_PAD_PAD_";
            int                 lines;

            lines = (cupsH - y) > STRIPE_HEIGHT ? STRIPE_HEIGHT : (cupsH - y);

            bitmaps[0] = buf + y * bpl;

            /* Adjust L0 for this stripe */
            if (lines < STRIPE_HEIGHT)
                JbgOptions[2] = lines;
            else
                JbgOptions[2] = STRIPE_HEIGHT;

            jbg_enc_init(&se, cupsW, lines, 1, bitmaps,
                         output_jbig, &chain);
            jbg_enc_options(&se, JbgOptions[0], JbgOptions[1],
                            JbgOptions[2], JbgOptions[3], JbgOptions[4]);
            jbg_enc_out(&se);
            jbg_enc_free(&se);

            if (!chain || chain->len != 20)
            {
                fprintf(stderr, "ERROR: rastertooak: missing BIH\n");
                break;
            }

            chainlen = 0;
            for (current = chain->next; current; current = current->next)
                chainlen += current->len;

            /* Build OAK_IMAGE_DATA record */
            memset(&recdata, 0, sizeof(recdata));

            /* Copy BIH and convert xd, yd, l0 to little-endian */
            memcpy(&recdata.bih, chain->data, sizeof(recdata.bih));
            iswap32(&recdata.bih.xd);
            iswap32(&recdata.bih.yd);
            iswap32(&recdata.bih.l0);

            recdata.datalen = chainlen;
            recdata.padlen = (recdata.datalen + 15) & ~0x0f;
            recdata.unk1C = 0;
            recdata.y = y;
            recdata.plane = 3;       /* K plane for HP 1500 */
            recdata.subplane = 0;

            oak_record(stdout, OAK_TYPE_IMAGE_DATA, &recdata, sizeof(recdata));

            /* Write JBIG BID data (skip BIH) */
            for (current = chain->next; current; current = current->next)
            {
                if (fwrite(current->data, 1, current->len, stdout) == 0)
                {
                    fprintf(stderr, "ERROR: rastertooak: write JBIG data failed\n");
                    break;
                }
            }

            /* Pad JBIG data to 16-byte boundary */
            padlen = recdata.padlen - recdata.datalen;
            if (padlen)
            {
                if (fwrite(pad, 1, padlen, stdout) == 0)
                {
                    fprintf(stderr, "ERROR: rastertooak: write pad failed\n");
                    break;
                }
            }

            free_chain(chain);
        }

        oak_record(stdout, OAK_TYPE_END_IMAGE, NULL, 0);

        endpage_arg = 0;  /* 0 = mono */
        oak_record(stdout, OAK_TYPE_END_PAGE, &endpage_arg, sizeof(endpage_arg));

        free(buf);

        fprintf(stderr, "PAGE: %d %d\n", page, copies);
    }

    end_doc(stdout);

    cupsRasterClose(ras);
    if (fd != 0) close(fd);

    return Canceled ? 1 : 0;
}
