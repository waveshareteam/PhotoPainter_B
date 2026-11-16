/*****************************************************************************
* | File      	:   GUI_JPGfile.c
* | Function    :   Decode a JPEG from the SD card straight into the 6-color
*                   frame buffer.
*
*   Pipeline (streamed, the full RGB image never sits in RAM):
*     SD --f_read--> TJpgDec (MCU blocks, optional 1/2..1/8 descale)
*        --> strip buffer (one MCU row) --> nearest-neighbour resample +
*        center crop --> Floyd-Steinberg (same palette as convert.py)
*        --> Paint_SetPixel()
*
*   RAM: ~16 KB decoder work area + strip (srcW * MCU height * 3, <= ~77 KB)
*        + 2 error rows (~10 KB), on top of the 192 KB frame buffer.
******************************************************************************/
#include "GUI_JPGfile.h"
#include "GUI_Paint.h"
#include "tjpgd.h"

#include "ff.h"
#include "f_util.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JPG_WORK_SIZE   16384   /* TJpgDec needs ~11 KB with JD_SZBUF 2048 + JD_FASTDECODE 2 */

/* Panel palette, index = EPD color code (4 is unused; same table as convert.py) */
static const uint8_t PALETTE[][3] = {
    {  0,   0,   0},    /* 0 black  */
    {255, 255, 255},    /* 1 white  */
    {255, 255,   0},    /* 2 yellow */
    {255,   0,   0},    /* 3 red    */
    {  0,   0,   0},    /* 4 (unused) */
    {  0,   0, 255},    /* 5 blue   */
    {  0, 255,   0},    /* 6 green  */
};
static const uint8_t PALETTE_USED[] = {0, 1, 2, 3, 5, 6};

typedef struct {
    FIL fil;
    uint16_t sw, sh;        /* decoded (descaled) source size */
    uint16_t tw, th;        /* target size: 800x480 or 480x800 */
    uint32_t num, den;      /* source pixels per target pixel = num / den */
    uint32_t x0, y0;        /* center-crop offset, in source pixels */
    uint16_t *sx_lut;       /* target x -> source x */
    uint8_t *strip;         /* one MCU row of decoded RGB888 */
    uint16_t next_oy;       /* next target row to produce */
    int16_t *err_cur;       /* FS error (x16) for the current row, 1 px padding each side */
    int16_t *err_next;
    uint8_t *row_rgb;       /* resampled target row */
    uint64_t t_read_us;     /* time spent in f_read */
    uint64_t t_dither_us;   /* time spent resampling + dithering */
} jpg_ctx_t;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
    if (buf) {
        uint64_t t0 = time_us_64();
        UINT br = 0;
        FRESULT fr = f_read(&c->fil, buf, n, &br);
        c->t_read_us += time_us_64() - t0;
        return (fr == FR_OK) ? br : 0;
    }
    return (f_lseek(&c->fil, f_tell(&c->fil) + n) == FR_OK) ? n : 0;
}

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Floyd-Steinberg one target row (left to right), write it into the frame buffer */
static void dither_row(jpg_ctx_t *c, uint16_t oy)
{
    int16_t *ec = c->err_cur, *en = c->err_next;
    memset(en, 0, (c->tw + 2) * 3 * sizeof(int16_t));
    const uint8_t *p = c->row_rgb;

    for (int x = 0; x < c->tw; x++, p += 3) {
        int16_t *e = &ec[(x + 1) * 3];
        int r = clamp255(p[0] + ((e[0] + 8) >> 4));
        int g = clamp255(p[1] + ((e[1] + 8) >> 4));
        int b = clamp255(p[2] + ((e[2] + 8) >> 4));

        int best = 0;
        int bestd = 0x7fffffff;
        for (unsigned i = 0; i < sizeof(PALETTE_USED); i++) {
            const uint8_t *q = PALETTE[PALETTE_USED[i]];
            int dr = r - q[0], dg = g - q[1], db = b - q[2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bestd) {
                bestd = d;
                best = PALETTE_USED[i];
            }
        }

        const uint8_t *q = PALETTE[best];
        int er[3] = {r - q[0], g - q[1], b - q[2]};
        for (int k = 0; k < 3; k++) {
            ec[(x + 2) * 3 + k] += er[k] * 7;   /* right        */
            en[(x + 0) * 3 + k] += er[k] * 3;   /* below-left   */
            en[(x + 1) * 3 + k] += er[k] * 5;   /* below        */
            en[(x + 2) * 3 + k] += er[k] * 1;   /* below-right  */
        }

        /* Same orientation as GUI_ReadBmp_RGB_6Color(): BMP rows are stored
           bottom-up and drawn with x mirrored, i.e. the image is rotated 180. */
        Paint_SetPixel(c->tw - 1 - x, c->th - 1 - oy, best);
    }

    c->err_cur = en;
    c->err_next = ec;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_ctx_t *c = (jpg_ctx_t *)jd->device;
    uint16_t bw = rect->right - rect->left + 1;
    uint16_t bh = rect->bottom - rect->top + 1;
    const uint8_t *src = (const uint8_t *)bitmap;

    /* All blocks of one MCU row share rect->top, so the strip is indexed from it */
    for (uint16_t r = 0; r < bh; r++) {
        memcpy(c->strip + ((uint32_t)r * c->sw + rect->left) * 3, src + (uint32_t)r * bw * 3, bw * 3);
    }

    if (rect->right != c->sw - 1) {
        return 1;   /* MCU row not complete yet */
    }

    /* MCU row complete: emit every target row whose source row lies in it */
    uint64_t t0 = time_us_64();
    uint32_t bottom = rect->bottom + 1;
    while (c->next_oy < c->th) {
        uint32_t sy = c->y0 + (uint32_t)c->next_oy * c->num / c->den;
        if (sy >= c->sh) sy = c->sh - 1;
        if (sy >= bottom) break;
        if (sy < rect->top) sy = rect->top;     /* cannot happen with monotonic mapping, be safe */

        const uint8_t *srow = c->strip + (sy - rect->top) * c->sw * 3;
        uint8_t *d = c->row_rgb;
        for (uint16_t x = 0; x < c->tw; x++, d += 3) {
            const uint8_t *s = srow + c->sx_lut[x] * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
        dither_row(c, c->next_oy);
        c->next_oy++;
        DEV_SetStage(STAGE_JPG_DECODE, c->next_oy);
    }
    c->t_dither_us += time_us_64() - t0;

    watchdog_update();
    return 1;
}

const char *GUI_JpgErrorStr(int err)
{
    switch (err) {
    case JDR_OK:        return "OK";
    case JDR_INTR:      return "interrupted";
    case JDR_INP:       return "read error / truncated file";
    case JDR_MEM1:      return "decoder work area too small";
    case JDR_MEM2:      return "input buffer too small";
    case JDR_PAR:       return "parameter error";
    case JDR_FMT1:      return "broken JPEG data";
    case JDR_FMT2:      return "unsupported JPEG variant";
    case JDR_FMT3:      return "unsupported JPEG (progressive?)";
    case JPG_ERR_OPEN:  return "cannot open file";
    case JPG_ERR_NOMEM: return "out of memory";
    default:            return "unknown error";
    }
}

int GUI_ReadJpg_RGB_6Color(const char *path)
{
    uint64_t t_start = time_us_64();
    int ret = 0;
    void *work = NULL;
    jpg_ctx_t *c = calloc(1, sizeof(jpg_ctx_t));
    if (!c) return JPG_ERR_NOMEM;

    printf("[JPG] open %s\r\n", path);
    FRESULT fr = f_open(&c->fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[JPG] f_open error: %s (%d)\r\n", FRESULT_str(fr), fr);
        free(c);
        return JPG_ERR_OPEN;
    }
    uint32_t fsize = (uint32_t)f_size(&c->fil);

    work = malloc(JPG_WORK_SIZE);
    if (!work) { ret = JPG_ERR_NOMEM; goto done; }

    JDEC jd;
    DEV_SetStage(STAGE_JPG_DECODE, 0);
    JRESULT jr = jd_prepare(&jd, jpg_in, work, JPG_WORK_SIZE, c);
    uint64_t t_prepared = time_us_64();
    c->t_read_us = 0;   /* header reads are counted in "open+header" */
    if (jr != JDR_OK) {
        printf("[JPG] jd_prepare error %d: %s\r\n", jr, GUI_JpgErrorStr(jr));
        ret = jr;
        goto done;
    }

    /* Orientation follows the source, like convert.py and the BMP reader */
    bool landscape = jd.width > jd.height;
    c->tw = landscape ? 800 : 480;
    c->th = landscape ? 480 : 800;
    Paint_SetRotate(landscape ? ROTATE_0 : ROTATE_90);

    /* Largest TJpgDec descale that still covers the target (only downscale) */
    uint8_t scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= c->tw && (jd.height >> (scale + 1)) >= c->th) {
        scale++;
    }
    c->sw = jd.width >> scale;
    c->sh = jd.height >> scale;

    /* Cover + center crop (convert.py default "scale" mode does the same) */
    if ((uint32_t)c->tw * c->sh >= (uint32_t)c->th * c->sw) {   /* source taller: width decides */
        c->num = c->sw; c->den = c->tw;
        uint32_t vh = (uint32_t)c->th * c->sw / c->tw;
        c->x0 = 0;
        c->y0 = (c->sh > vh) ? (c->sh - vh) / 2 : 0;
    } else {                                                    /* source wider: height decides */
        c->num = c->sh; c->den = c->th;
        uint32_t vw = (uint32_t)c->tw * c->sh / c->th;
        c->y0 = 0;
        c->x0 = (c->sw > vw) ? (c->sw - vw) / 2 : 0;
    }

    uint16_t strip_h = (uint16_t)(jd.msy * 8) >> scale;
    if (strip_h == 0) strip_h = 1;
    c->strip    = malloc((size_t)c->sw * strip_h * 3);
    c->sx_lut   = malloc(c->tw * sizeof(uint16_t));
    c->row_rgb  = malloc(c->tw * 3);
    c->err_cur  = calloc((c->tw + 2) * 3, sizeof(int16_t));
    c->err_next = calloc((c->tw + 2) * 3, sizeof(int16_t));
    if (!c->strip || !c->sx_lut || !c->row_rgb || !c->err_cur || !c->err_next) {
        printf("[JPG] malloc failed (strip %u bytes)\r\n", (unsigned)(c->sw * strip_h * 3));
        ret = JPG_ERR_NOMEM;
        goto done;
    }
    for (uint16_t x = 0; x < c->tw; x++) {
        uint32_t sx = c->x0 + (uint32_t)x * c->num / c->den;
        c->sx_lut[x] = (uint16_t)((sx >= c->sw) ? (uint32_t)c->sw - 1 : sx);
    }

    printf("[JPG] %lu bytes, %ux%u, MCU %ux%u -> descale 1/%d (%ux%u) -> %ux%u, crop at (%lu,%lu)\r\n",
           (unsigned long)fsize, jd.width, jd.height, jd.msx * 8, jd.msy * 8, 1 << scale,
           c->sw, c->sh, c->tw, c->th, (unsigned long)c->x0, (unsigned long)c->y0);

    jr = jd_decomp(&jd, jpg_out, scale);
    if (jr != JDR_OK) {
        printf("[JPG] jd_decomp error %d: %s (row %u/%u)\r\n", jr, GUI_JpgErrorStr(jr), c->next_oy, c->th);
        ret = jr;
        goto done;
    }
    if (c->next_oy < c->th) {
        printf("[JPG] warning: only %u/%u rows produced\r\n", c->next_oy, c->th);
    }

    uint64_t t_end = time_us_64();
    uint32_t ms_total   = (uint32_t)((t_end - t_start) / 1000);
    uint32_t ms_prepare = (uint32_t)((t_prepared - t_start) / 1000);
    uint32_t ms_read    = (uint32_t)(c->t_read_us / 1000);
    uint32_t ms_dither  = (uint32_t)(c->t_dither_us / 1000);
    uint32_t ms_decode  = ms_total - ms_prepare;
    ms_decode = (ms_decode > ms_read + ms_dither) ? ms_decode - ms_read - ms_dither : 0;
    printf("[JPG] time: total %lu ms = open+header %lu + SD read %lu + decode %lu + resample/dither %lu\r\n",
           (unsigned long)ms_total, (unsigned long)ms_prepare, (unsigned long)ms_read,
           (unsigned long)ms_decode, (unsigned long)ms_dither);

done:
    f_close(&c->fil);
    free(work);
    free(c->strip);
    free(c->sx_lut);
    free(c->row_rgb);
    free(c->err_cur);
    free(c->err_next);
    free(c);
    return ret;
}
