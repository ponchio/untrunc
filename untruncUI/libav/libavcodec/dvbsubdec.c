/*
 * DVB subtitle decoding
 * Copyright (c) 2005 Ian Caulfield
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "bitstream.h"
#include "bytestream.h"
#include "internal.h"
#include "libavutil/colorspace.h"

#define DVBSUB_PAGE_SEGMENT     0x10
#define DVBSUB_REGION_SEGMENT   0x11
#define DVBSUB_CLUT_SEGMENT     0x12
#define DVBSUB_OBJECT_SEGMENT   0x13
#define DVBSUB_DISPLAYDEFINITION_SEGMENT 0x14
#define DVBSUB_DISPLAY_SEGMENT  0x80

#define cm (ff_crop_tab + MAX_NEG_CROP)

#define RGBA(r,g,b,a) (((unsigned)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))

typedef struct DVBSubCLUT {
    int id;

    uint32_t clut4[4];
    uint32_t clut16[16];
    uint32_t clut256[256];

    struct DVBSubCLUT *next;
} DVBSubCLUT;

static DVBSubCLUT default_clut;

typedef struct DVBSubObjectDisplay {
    int object_id;
    int region_id;

    int x_pos;
    int y_pos;

    int fgcolor;
    int bgcolor;

    struct DVBSubObjectDisplay *region_list_next;
    struct DVBSubObjectDisplay *object_list_next;
} DVBSubObjectDisplay;

typedef struct DVBSubObject {
    int id;

    int type;

    DVBSubObjectDisplay *display_list;

    struct DVBSubObject *next;
} DVBSubObject;

typedef struct DVBSubRegionDisplay {
    int region_id;

    int x_pos;
    int y_pos;

    struct DVBSubRegionDisplay *next;
} DVBSubRegionDisplay;

typedef struct DVBSubRegion {
    int id;

    int width;
    int height;
    int depth;

    int clut;
    int bgcolor;

    uint8_t *pbuf;
    int buf_size;

    DVBSubObjectDisplay *display_list;

    struct DVBSubRegion *next;
} DVBSubRegion;

typedef struct DVBSubDisplayDefinition {
    int version;

    int x;
    int y;
    int width;
    int height;
} DVBSubDisplayDefinition;

typedef struct DVBSubContext {
    int composition_id;
    int ancillary_id;

    int time_out;
    DVBSubRegion *region_list;
    DVBSubCLUT   *clut_list;
    DVBSubObject *object_list;

    int display_list_size;
    DVBSubRegionDisplay *display_list;
    DVBSubDisplayDefinition *display_definition;
} DVBSubContext;


static DVBSubObject* get_object(DVBSubContext *ctx, int object_id)
{
    DVBSubObject *ptr = ctx->object_list;

    while (ptr && ptr->id != object_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static DVBSubCLUT* get_clut(DVBSubContext *ctx, int clut_id)
{
    DVBSubCLUT *ptr = ctx->clut_list;

    while (ptr && ptr->id != clut_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static DVBSubRegion* get_region(DVBSubContext *ctx, int region_id)
{
    DVBSubRegion *ptr = ctx->region_list;

    while (ptr && ptr->id != region_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static void delete_region_display_list(DVBSubContext *ctx, DVBSubRegion *region)
{
    DVBSubObject *object, *obj2, **obj2_ptr;
    DVBSubObjectDisplay *display, *obj_disp, **obj_disp_ptr;

    while (region->display_list) {
        display = region->display_list;

        object = get_object(ctx, display->object_id);

        if (object) {
            obj_disp_ptr = &object->display_list;
            obj_disp = *obj_disp_ptr;

            while (obj_disp && obj_disp != display) {
                obj_disp_ptr = &obj_disp->object_list_next;
                obj_disp = *obj_disp_ptr;
            }

            if (obj_disp) {
                *obj_disp_ptr = obj_disp->object_list_next;

                if (!object->display_list) {
                    obj2_ptr = &ctx->object_list;
                    obj2 = *obj2_ptr;

                    while (obj2 != object) {
                        assert(obj2);
                        obj2_ptr = &obj2->next;
                        obj2 = *obj2_ptr;
                    }

                    *obj2_ptr = obj2->next;

                    av_free(obj2);
                }
            }
        }

        region->display_list = display->region_list_next;

        av_free(display);
    }

}

static void delete_state(DVBSubContext *ctx)
{
    DVBSubRegion *region;
    DVBSubCLUT *clut;

    while (ctx->region_list) {
        region = ctx->region_list;

        ctx->region_list = region->next;

        delete_region_display_list(ctx, region);
        av_free(region->pbuf);
        av_free(region);
    }

    while (ctx->clut_list) {
        clut = ctx->clut_list;

        ctx->clut_list = clut->next;

        av_free(clut);
    }

    av_freep(&ctx->display_definition);

    /* Should already be null */
    if (ctx->object_list)
        av_log(NULL, AV_LOG_ERROR, "Memory deallocation error!\n");
}

static av_cold int dvbsub_init_decoder(AVCodecContext *avctx)
{
    int i, r, g, b, a = 0;
    DVBSubContext *ctx = avctx->priv_data;

    if (!avctx->extradata || avctx->extradata_size != 4) {
        av_log(avctx, AV_LOG_WARNING, "Invalid extradata, subtitle streams may be combined!\n");
        ctx->composition_id = -1;
        ctx->ancillary_id   = -1;
    } else {
        ctx->composition_id = AV_RB16(avctx->extradata);
        ctx->ancillary_id   = AV_RB16(avctx->extradata + 2);
    }

    default_clut.id = -1;
    default_clut.next = NULL;

    default_clut.clut4[0] = RGBA(  0,   0,   0,   0);
    default_clut.clut4[1] = RGBA(255, 255, 255, 255);
    default_clut.clut4[2] = RGBA(  0,   0,   0, 255);
    default_clut.clut4[3] = RGBA(127, 127, 127, 255);

    default_clut.clut16[0] = RGBA(  0,   0,   0,   0);
    for (i = 1; i < 16; i++) {
        if (i < 8) {
            r = (i & 1) ? 255 : 0;
            g = (i & 2) ? 255 : 0;
            b = (i & 4) ? 255 : 0;
        } else {
            r = (i & 1) ? 127 : 0;
            g = (i & 2) ? 127 : 0;
            b = (i & 4) ? 127 : 0;
        }
        default_clut.clut16[i] = RGBA(r, g, b, 255);
    }

    default_clut.clut256[0] = RGBA(  0,   0,   0,   0);
    for (i = 1; i < 256; i++) {
        if (i < 8) {
            r = (i & 1) ? 255 : 0;
            g = (i & 2) ? 255 : 0;
            b = (i & 4) ? 255 : 0;
            a = 63;
        } else {
            switch (i & 0x88) {
            case 0x00:
                r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
                g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
                b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
                a = 255;
                break;
            case 0x08:
                r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
                g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
                b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
                a = 127;
                break;
            case 0x80:
                r = 127 + ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = 127 + ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = 127 + ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            case 0x88:
                r = ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            }
        }
        default_clut.clut256[i] = RGBA(r, g, b, a);
    }

    return 0;
}

static av_cold int dvbsub_close_decoder(AVCodecContext *avctx)
{
    DVBSubContext *ctx = avctx->priv_data;
    DVBSubRegionDisplay *display;

    delete_state(ctx);

    while (ctx->display_list) {
        display = ctx->display_list;
        ctx->display_list = display->next;

        av_free(display);
    }

    return 0;
}

static int dvbsub_read_2bit_string(uint8_t *destbuf, int dbuf_len,
                                   const uint8_t **srcbuf, int buf_size,
                                   int non_mod, uint8_t *map_table)
{
    BitstreamContext bc;

    int bits;
    int run_length;
    int pixels_read = 0;

    bitstream_init8(&bc, *srcbuf, buf_size);

    while (bitstream_tell(&bc) < buf_size << 3 && pixels_read < dbuf_len) {
        bits = bitstream_read(&bc, 2);

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = bitstream_read_bit(&bc);
            if (bits == 1) {
                run_length = bitstream_read(&bc, 3) + 3;
                bits       = bitstream_read(&bc, 2);

                if (non_mod == 1 && bits == 1)
                    pixels_read += run_length;
                else {
                    if (map_table)
                        bits = map_table[bits];
                    while (run_length-- > 0 && pixels_read < dbuf_len) {
                        *destbuf++ = bits;
                        pixels_read++;
                    }
                }
            } else {
                bits = bitstream_read_bit(&bc);
                if (bits == 0) {
                    bits = bitstream_read(&bc, 2);
                    if (bits == 2) {
                        run_length = bitstream_read(&bc, 4) + 12;
                        bits       = bitstream_read(&bc, 2);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 3) {
                        run_length = bitstream_read(&bc, 8) + 29;
                        bits = bitstream_read(&bc, 2);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 1) {
                        pixels_read += 2;
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        if (pixels_read <= dbuf_len) {
                            *destbuf++ = bits;
                            *destbuf++ = bits;
                        }
                    } else {
                        *srcbuf += (bitstream_tell(&bc) + 7) >> 3;
                        return pixels_read;
                    }
                } else {
                    if (map_table)
                        bits = map_table[0];
                    else
                        bits = 0;
                    *destbuf++ = bits;
                    pixels_read++;
                }
            }
        }
    }

    if (bitstream_read(&bc, 6))
        av_log(NULL, AV_LOG_ERROR, "DVBSub error: line overflow\n");

    *srcbuf += (bitstream_tell(&bc) + 7) >> 3;

    return pixels_read;
}

static int dvbsub_read_4bit_string(uint8_t *destbuf, int dbuf_len,
                                   const uint8_t **srcbuf, int buf_size,
                                   int non_mod, uint8_t *map_table)
{
    BitstreamContext bc;

    int bits;
    int run_length;
    int pixels_read = 0;

    bitstream_init8(&bc, *srcbuf, buf_size);

    while (bitstream_tell(&bc) < buf_size << 3 && pixels_read < dbuf_len) {
        bits = bitstream_read(&bc, 4);

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = bitstream_read_bit(&bc);
            if (bits == 0) {
                run_length = bitstream_read(&bc, 3);

                if (run_length == 0) {
                    *srcbuf += (bitstream_tell(&bc) + 7) >> 3;
                    return pixels_read;
                }

                run_length += 2;

                if (map_table)
                    bits = map_table[0];
                else
                    bits = 0;

                while (run_length-- > 0 && pixels_read < dbuf_len) {
                    *destbuf++ = bits;
                    pixels_read++;
                }
            } else {
                bits = bitstream_read_bit(&bc);
                if (bits == 0) {
                    run_length = bitstream_read(&bc, 2) + 4;
                    bits       = bitstream_read(&bc, 4);

                    if (non_mod == 1 && bits == 1)
                        pixels_read += run_length;
                    else {
                        if (map_table)
                            bits = map_table[bits];
                        while (run_length-- > 0 && pixels_read < dbuf_len) {
                            *destbuf++ = bits;
                            pixels_read++;
                        }
                    }
                } else {
                    bits = bitstream_read(&bc, 2);
                    if (bits == 2) {
                        run_length = bitstream_read(&bc, 4) + 9;
                        bits       = bitstream_read(&bc, 4);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 3) {
                        run_length = bitstream_read(&bc, 8) + 25;
                        bits = bitstream_read(&bc, 4);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 1) {
                        pixels_read += 2;
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        if (pixels_read <= dbuf_len) {
                            *destbuf++ = bits;
                            *destbuf++ = bits;
                        }
                    } else {
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        *destbuf++ = bits;
                        pixels_read ++;
                    }
                }
            }
        }
    }

    if (bitstream_read(&bc, 8))
        av_log(NULL, AV_LOG_ERROR, "DVBSub error: line overflow\n");

    *srcbuf += (bitstream_tell(&bc) + 7) >> 3;

    return pixels_read;
}

static int dvbsub_read_8bit_string(uint8_t *destbuf, int dbuf_len,
                                    const uint8_t **srcbuf, int buf_size,
                                    int non_mod, uint8_t *map_table)
{
    const uint8_t *sbuf_end = (*srcbuf) + buf_size;
    int bits;
    int run_length;
    int pixels_read = 0;

    while (*srcbuf < sbuf_end && pixels_read < dbuf_len) {
        bits = *(*srcbuf)++;

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = *(*srcbuf)++;
            run_length = bits & 0x7f;
            if ((bits & 0x80) == 0) {
                if (run_length == 0) {
                    return pixels_read;
                }
            } else {
                bits = *(*srcbuf)++;

                if (non_mod == 1 && bits == 1)
                    pixels_read += run_length;
            }
            if (map_table)
                bits = map_table[0];
            else
                bits = 0;
            while (run_length-- > 0 && pixels_read < dbuf_len) {
                *destbuf++ = bits;
                pixels_read++;
            }
        }
    }

    if (*(*srcbuf)++)
        av_log(NULL, AV_LOG_ERROR, "DVBSub error: line overflow\n");

    return pixels_read;
}



static void dvbsub_parse_pixel_data_block(AVCodecContext *avctx, DVBSubObjectDisplay *display,
                                          const uint8_t *buf, int buf_size, int top_bottom, int non_mod)
{
    DVBSubContext *ctx = avctx->priv_data;

    DVBSubRegion *region = get_region(ctx, display->region_id);
    const uint8_t *buf_end = buf + buf_size;
    uint8_t *pbuf;
    int x_pos, y_pos;
    int i;

    uint8_t map2to4[] = { 0x0,  0x7,  0x8,  0xf};
    uint8_t map2to8[] = {0x00, 0x77, 0x88, 0xff};
    uint8_t map4to8[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t *map_table;

    ff_dlog(avctx, "DVB pixel block size %d, %s field:\n", buf_size,
            top_bottom ? "bottom" : "top");

    for (i = 0; i < buf_size; i++) {
        if (i % 16 == 0)
            ff_dlog(avctx, "0x%8p: ", buf+i);

        ff_dlog(avctx, "%02x ", buf[i]);
        if (i % 16 == 15)
            ff_dlog(avctx, "\n");
    }

    if (i % 16)
        ff_dlog(avctx, "\n");

    if (region == 0)
        return;

    pbuf = region->pbuf;

    x_pos = display->x_pos;
    y_pos = display->y_pos;

    if ((y_pos & 1) != top_bottom)
        y_pos++;

    while (buf < buf_end) {
        if (x_pos > region->width || y_pos > region->height) {
            av_log(avctx, AV_LOG_ERROR, "Invalid object location!\n");
            return;
        }

        switch (*buf++) {
        case 0x10:
            if (region->depth == 8)
                map_table = map2to8;
            else if (region->depth == 4)
                map_table = map2to4;
            else
                map_table = NULL;

            x_pos += dvbsub_read_2bit_string(pbuf + (y_pos * region->width) + x_pos,
                                                region->width - x_pos, &buf, buf_end - buf,
                                                non_mod, map_table);
            break;
        case 0x11:
            if (region->depth < 4) {
                av_log(avctx, AV_LOG_ERROR, "4-bit pixel string in %d-bit region!\n", region->depth);
                return;
            }

            if (region->depth == 8)
                map_table = map4to8;
            else
                map_table = NULL;

            x_pos += dvbsub_read_4bit_string(pbuf + (y_pos * region->width) + x_pos,
                                                region->width - x_pos, &buf, buf_end - buf,
                                                non_mod, map_table);
            break;
        case 0x12:
            if (region->depth < 8) {
                av_log(avctx, AV_LOG_ERROR, "8-bit pixel string in %d-bit region!\n", region->depth);
                return;
            }

            x_pos += dvbsub_read_8bit_string(pbuf + (y_pos * region->width) + x_pos,
                                                region->width - x_pos, &buf, buf_end - buf,
                                                non_mod, NULL);
            break;

        case 0x20:
            map2to4[0] = (*buf) >> 4;
            map2to4[1] = (*buf++) & 0xf;
            map2to4[2] = (*buf) >> 4;
            map2to4[3] = (*buf++) & 0xf;
            break;
        case 0x21:
            for (i = 0; i < 4; i++)
                map2to8[i] = *buf++;
            break;
        case 0x22:
            for (i = 0; i < 16; i++)
                map4to8[i] = *buf++;
            break;

        case 0xf0:
            x_pos = display->x_pos;
            y_pos += 2;
            break;
        default:
            av_log(avctx, AV_LOG_INFO, "Unknown/unsupported pixel block 0x%x\n", *(buf-1));
        }
    }

}

static int dvbsub_parse_object_segment(AVCodecContext *avctx,
                                       const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = avctx->priv_data;

    const uint8_t *buf_end = buf + buf_size;
    const uint8_t *block;
    int object_id;
    DVBSubObject *object;
    DVBSubObjectDisplay *display;
    int top_field_len, bottom_field_len;

    int coding_method, non_modifying_color;

    object_id = AV_RB16(buf);
    buf += 2;

    object = get_object(ctx, object_id);

    if (!object)
        return AVERROR_INVALIDDATA;

    coding_method = ((*buf) >> 2) & 3;
    non_modifying_color = ((*buf++) >> 1) & 1;

    if (coding_method == 0) {
        top_field_len = AV_RB16(buf);
        buf += 2;
        bottom_field_len = AV_RB16(buf);
        buf += 2;

        if (buf + top_field_len + bottom_field_len > buf_end) {
            av_log(avctx, AV_LOG_ERROR, "Field data size too large\n");
            return AVERROR_INVALIDDATA;
        }

        for (display = object->display_list; display; display = display->object_list_next) {
            block = buf;

            dvbsub_parse_pixel_data_block(avctx, display, block, top_field_len, 0,
                                            non_modifying_color);

            if (bottom_field_len > 0)
                block = buf + top_field_len;
            else
                bottom_field_len = top_field_len;

            dvbsub_parse_pixel_data_block(avctx, display, block, bottom_field_len, 1,
                                            non_modifying_color);
        }

/*  } else if (coding_method == 1) {*/

    } else {
        av_log(avctx, AV_LOG_ERROR, "Unknown object coding %d\n", coding_method);
    }

    return 0;
}

static int dvbsub_parse_clut_segment(AVCodecContext *avctx,
                                     const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = avctx->priv_data;

    const uint8_t *buf_end = buf + buf_size;
    int i, clut_id;
    DVBSubCLUT *clut;
    int entry_id, depth , full_range;
    int y, cr, cb, alpha;
    int r, g, b, r_add, g_add, b_add;

    ff_dlog(avctx, "DVB clut packet:\n");

    for (i=0; i < buf_size; i++) {
        ff_dlog(avctx, "%02x ", buf[i]);
        if (i % 16 == 15)
            ff_dlog(avctx, "\n");
    }

    if (i % 16)
        ff_dlog(avctx, "\n");

    clut_id = *buf++;
    buf += 1;

    clut = get_clut(ctx, clut_id);

    if (!clut) {
        clut = av_malloc(sizeof(DVBSubCLUT));
        if (!clut)
            return AVERROR(ENOMEM);

        memcpy(clut, &default_clut, sizeof(DVBSubCLUT));

        clut->id = clut_id;

        clut->next = ctx->clut_list;
        ctx->clut_list = clut;
    }

    while (buf + 4 < buf_end) {
        entry_id = *buf++;

        depth = (*buf) & 0xe0;

        if (depth == 0) {
            av_log(avctx, AV_LOG_ERROR, "Invalid clut depth 0x%x!\n", *buf);
            return AVERROR_INVALIDDATA;
        }

        full_range = (*buf++) & 1;

        if (full_range) {
            y = *buf++;
            cr = *buf++;
            cb = *buf++;
            alpha = *buf++;
        } else {
            y = buf[0] & 0xfc;
            cr = (((buf[0] & 3) << 2) | ((buf[1] >> 6) & 3)) << 4;
            cb = (buf[1] << 2) & 0xf0;
            alpha = (buf[1] << 6) & 0xc0;

            buf += 2;
        }

        if (y == 0)
            alpha = 0xff;

        YUV_TO_RGB1_CCIR(cb, cr);
        YUV_TO_RGB2_CCIR(r, g, b, y);

        ff_dlog(avctx, "clut %d := (%d,%d,%d,%d)\n", entry_id, r, g, b, alpha);

        if (depth & 0x80)
            clut->clut4[entry_id] = RGBA(r,g,b,255 - alpha);
        if (depth & 0x40)
            clut->clut16[entry_id] = RGBA(r,g,b,255 - alpha);
        if (depth & 0x20)
            clut->clut256[entry_id] = RGBA(r,g,b,255 - alpha);
    }

    return 0;
}


static int dvbsub_parse_region_segment(AVCodecContext *avctx,
                                       const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = avctx->priv_data;

    const uint8_t *buf_end = buf + buf_size;
    int region_id, object_id;
    DVBSubRegion *region;
    DVBSubObject *object;
    DVBSubObjectDisplay *display;
    int fill;

    if (buf_size < 10)
        return AVERROR_INVALIDDATA;

    region_id = *buf++;

    region = get_region(ctx, region_id);

    if (!region) {
        region = av_mallocz(sizeof(DVBSubRegion));
        if (!region)
            return AVERROR(ENOMEM);

        region->id = region_id;

        region->next = ctx->region_list;
        ctx->region_list = region;
    }

    fill = ((*buf++) >> 3) & 1;

    region->width = AV_RB16(buf);
    buf += 2;
    region->height = AV_RB16(buf);
    buf += 2;

    if (region->width * region->height != region->buf_size) {
        av_free(region->pbuf);

        region->buf_size = region->width * region->height;

        region->pbuf = av_malloc(region->buf_size);
        if (!region->pbuf)
            return AVERROR(ENOMEM);

        fill = 1;
    }

    region->depth = 1 << (((*buf++) >> 2) & 7);
    if(region->depth<2 || region->depth>8){
        av_log(avctx, AV_LOG_ERROR, "region depth %d is invalid\n", region->depth);
        region->depth= 4;
    }
    region->clut = *buf++;

    if (region->depth == 8)
        region->bgcolor = *buf++;
    else {
        buf += 1;

        if (region->depth == 4)
            region->bgcolor = (((*buf++) >> 4) & 15);
        else
            region->bgcolor = (((*buf++) >> 2) & 3);
    }

    ff_dlog(avctx, "Region %d, (%dx%d)\n", region_id, region->width, region->height);

    if (fill) {
        memset(region->pbuf, region->bgcolor, region->buf_size);
        ff_dlog(avctx, "Fill region (%d)\n", region->bgcolor);
    }

    delete_region_display_list(ctx, region);

    while (buf + 5 < buf_end) {
        object_id = AV_RB16(buf);
        buf += 2;

        object = get_object(ctx, object_id);

        if (!object) {
            object = av_mallocz(sizeof(DVBSubObject));
            if (!object)
                return AVERROR(ENOMEM);

            object->id = object_id;
            object->next = ctx->object_list;
            ctx->object_list = object;
        }

        object->type = (*buf) >> 6;

        display = av_mallocz(sizeof(DVBSubObjectDisplay));
        if (!display)
            return AVERROR(ENOMEM);

        display->object_id = object_id;
        display->region_id = region_id;

        display->x_pos = AV_RB16(buf) & 0xfff;
        buf += 2;
        display->y_pos = AV_RB16(buf) & 0xfff;
        buf += 2;

        if ((object->type == 1 || object->type == 2) && buf+1 < buf_end) {
            display->fgcolor = *buf++;
            display->bgcolor = *buf++;
        }

        display->region_list_next = region->display_list;
        region->display_list = display;

        display->object_list_next = object->display_list;
        object->display_list = display;
    }

    return 0;
}

static int dvbsub_parse_page_segment(AVCodecContext *avctx,
                                     const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = avctx->priv_data;
    DVBSubRegionDisplay *display;
    DVBSubRegionDisplay *tmp_display_list, **tmp_ptr;

    const uint8_t *buf_end = buf + buf_size;
    int region_id;
    int page_state;

    if (buf_size < 1)
        return AVERROR_INVALIDDATA;

    ctx->time_out = *buf++;
    page_state = ((*buf++) >> 2) & 3;

    ff_dlog(avctx, "Page time out %ds, state %d\n", ctx->time_out, page_state);

    if (page_state == 2) {
        delete_state(ctx);
    }

    tmp_display_list = ctx->display_list;
    ctx->display_list = NULL;
    ctx->display_list_size = 0;

    while (buf + 5 < buf_end) {
        region_id = *buf++;
        buf += 1;

        display = tmp_display_list;
        tmp_ptr = &tmp_display_list;

        while (display && display->region_id != region_id) {
            tmp_ptr = &display->next;
            display = display->next;
        }

        if (!display) {
            display = av_mallocz(sizeof(DVBSubRegionDisplay));
            if (!display)
                return AVERROR(ENOMEM);
        }

        display->region_id = region_id;

        display->x_pos = AV_RB16(buf);
        buf += 2;
        display->y_pos = AV_RB16(buf);
        buf += 2;

        *tmp_ptr = display->next;

        display->next = ctx->display_list;
        ctx->display_list = display;
        ctx->display_list_size++;

        ff_dlog(avctx, "Region %d, (%d,%d)\n", region_id, display->x_pos, display->y_pos);
    }

    while (tmp_display_list) {
        display = tmp_display_list;

        tmp_display_list = display->next;

        av_free(display);
    }

    return 0;
}


#ifdef DEBUG
static void png_save(const char *filename, uint32_t *bitmap, int w, int h)
{
    int x, y, v;
    FILE *f;
    char fname[40], fname2[40];
    char command[1024];

    snprintf(fname, sizeof(fname), "%s.ppm", filename);

    f = fopen(fname, "w");
    if (!f) {
        perror(fname);
        return;
    }
    fprintf(f, "P6\n"
            "%d %d\n"
            "%d\n",
            w, h, 255);
    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {
            v = bitmap[y * w + x];
            putc((v >> 16) & 0xff, f);
            putc((v >> 8) & 0xff, f);
            putc((v >> 0) & 0xff, f);
        }
    }
    fclose(f);


    snprintf(fname2, sizeof(fname2), "%s-a.pgm", filename);

    f = fopen(fname2, "w");
    if (!f) {
        perror(fname2);
        return;
    }
    fprintf(f, "P5\n"
            "%d %d\n"
            "%d\n",
            w, h, 255);
    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {
            v = bitmap[y * w + x];
            putc((v >> 24) & 0xff, f);
        }
    }
    fclose(f);

    snprintf(command, sizeof(command), "pnmtopng -alpha %s %s > %s.png 2> /dev/null", fname2, fname, filename);
    if (system(command) != 0) {
        printf("Error running pnmtopng\n");
        return;
    }

    snprintf(command, sizeof(command), "rm %s %s", fname, fname2);
    if (system(command) != 0) {
        printf("Error removing %s and %s\n", fname, fname2);
        return;
    }
}

static int save_display_set(DVBSubContext *ctx)
{
    DVBSubRegion *region;
    DVBSubRegionDisplay *display;
    DVBSubCLUT *clut;
    uint32_t *clut_table;
    int x_pos, y_pos, width, height;
    int x, y, y_off, x_off;
    uint32_t *pbuf;
    char filename[32];
    static int fileno_index = 0;

    x_pos = -1;
    y_pos = -1;
    width = 0;
    height = 0;

    for (display = ctx->display_list; display; display = display->next) {
        region = get_region(ctx, display->region_id);

        if (x_pos == -1) {
            x_pos = display->x_pos;
            y_pos = display->y_pos;
            width = region->width;
            height = region->height;
        } else {
            if (display->x_pos < x_pos) {
                width += (x_pos - display->x_pos);
                x_pos = display->x_pos;
            }

            if (display->y_pos < y_pos) {
                height += (y_pos - display->y_pos);
                y_pos = display->y_pos;
            }

            if (display->x_pos + region->width > x_pos + width) {
                width = display->x_pos + region->width - x_pos;
            }

            if (display->y_pos + region->height > y_pos + height) {
                height = display->y_pos + region->height - y_pos;
            }
        }
    }

    if (x_pos >= 0) {

        pbuf = av_malloc(width * height * 4);
        if (!pbuf)
            return AVERROR(ENOMEM);

        for (display = ctx->display_list; display; display = display->next) {
            region = get_region(ctx, display->region_id);

            x_off = display->x_pos - x_pos;
            y_off = display->y_pos - y_pos;

            clut = get_clut(ctx, region->clut);

            if (clut == 0)
                clut = &default_clut;

            switch (region->depth) {
            case 2:
                clut_table = clut->clut4;
                break;
            case 8:
                clut_table = clut->clut256;
                break;
            case 4:
            default:
                clut_table = clut->clut16;
                break;
            }

            for (y = 0; y < region->height; y++) {
                for (x = 0; x < region->width; x++) {
                    pbuf[((y + y_off) * width) + x_off + x] =
                        clut_table[region->pbuf[y * region->width + x]];
                }
            }

        }

        snprintf(filename, sizeof(filename), "dvbs.%d", fileno_index);

        png_save(filename, pbuf, width, height);

        av_free(pbuf);
    }

    fileno_index++;
    return 0;
}
#endif /* DEBUG */

static int dvbsub_parse_display_definition_segment(AVCodecContext *avctx,
                                                   const uint8_t *buf,
                                                   int buf_size)
{
    DVBSubContext *ctx = avctx->priv_data;
    DVBSubDisplayDefinition *display_def = ctx->display_definition;
    int dds_version, info_byte;

    if (buf_size < 5)
        return AVERROR_INVALIDDATA;

    info_byte   = bytestream_get_byte(&buf);
    dds_version = info_byte >> 4;
    if (display_def && display_def->version == dds_version)
        return 0; // already have this display definition version

    if (!display_def) {
        display_def             = av_mallocz(sizeof(*display_def));
        if (!display_def)
            return AVERROR(ENOMEM);
        ctx->display_definition = display_def;
    }

    display_def->version = dds_version;
    display_def->x       = 0;
    display_def->y       = 0;
    display_def->width   = bytestream_get_be16(&buf) + 1;
    display_def->height  = bytestream_get_be16(&buf) + 1;

    if (buf_size < 13)
        return AVERROR_INVALIDDATA;

    if (info_byte & 1<<3) { // display_window_flag
        display_def->x = bytestream_get_be16(&buf);
        display_def->y = bytestream_get_be16(&buf);
        display_def->width  = bytestream_get_be16(&buf) - display_def->x + 1;
        display_def->height = bytestream_get_be16(&buf) - display_def->y + 1;
    }

    return 0;
}

static int dvbsub_display_end_segment(AVCodecContext *avctx, const uint8_t *buf,
                                      int buf_size, AVSubtitle *sub)
{
    DVBSubContext *ctx = avctx->priv_data;
    DVBSubDisplayDefinition *display_def = ctx->display_definition;

    DVBSubRegion *region;
    DVBSubRegionDisplay *display;
    AVSubtitleRect *rect;
    DVBSubCLUT *clut;
    uint32_t *clut_table;
    int i;
    int offset_x=0, offset_y=0;

    sub->rects = NULL;
    sub->start_display_time = 0;
    sub->end_display_time = ctx->time_out * 1000;
    sub->format = 0;

    if (display_def) {
        offset_x = display_def->x;
        offset_y = display_def->y;
    }

    sub->num_rects = ctx->display_list_size;

    if (sub->num_rects > 0) {
        sub->rects = av_mallocz(sizeof(*sub->rects) * sub->num_rects);
        if (!sub->rects)
            return AVERROR(ENOMEM);
        for (i = 0; i < sub->num_rects; i++) {
            sub->rects[i] = av_mallocz(sizeof(*sub->rects[i]));
            if (!sub->rects[i]) {
                int j;
                for (j = 0; j < i; j ++)
                    av_free(sub->rects[j]);
                av_free(sub->rects);
                return AVERROR(ENOMEM);
            }
        }
    }

    i = 0;

    for (display = ctx->display_list; display; display = display->next) {
        region = get_region(ctx, display->region_id);
        rect = sub->rects[i];

        if (!region)
            continue;

        rect->x = display->x_pos + offset_x;
        rect->y = display->y_pos + offset_y;
        rect->w = region->width;
        rect->h = region->height;
        rect->nb_colors = 16;
        rect->type      = SUBTITLE_BITMAP;
        rect->linesize[0] = region->width;

        clut = get_clut(ctx, region->clut);

        if (!clut)
            clut = &default_clut;

        switch (region->depth) {
        case 2:
            clut_table = clut->clut4;
            break;
        case 8:
            clut_table = clut->clut256;
            break;
        case 4:
        default:
            clut_table = clut->clut16;
            break;
        }

        rect->data[1] = av_mallocz(AVPALETTE_SIZE);
        if (!rect->data[1]) {
            for (i = 0; i < sub->num_rects; i++)
                av_free(sub->rects[i]);
            av_free(sub->rects);
            return AVERROR(ENOMEM);
        }
        memcpy(rect->data[1], clut_table, (1 << region->depth) * sizeof(uint32_t));

        rect->data[0] = av_malloc(region->buf_size);
        if (!rect->data[0]) {
            av_free(rect->data[1]);
            for (i = 0; i < sub->num_rects; i++)
                av_free(sub->rects[i]);
            av_free(sub->rects);
            return AVERROR(ENOMEM);
        }
        memcpy(rect->data[0], region->pbuf, region->buf_size);

#if FF_API_AVPICTURE
FF_DISABLE_DEPRECATION_WARNINGS
{
        int j;
        for (j = 0; j < 4; j++) {
            rect->pict.data[j] = rect->data[j];
            rect->pict.linesize[j] = rect->linesize[j];
        }
}
FF_ENABLE_DEPRECATION_WARNINGS
#endif

        i++;
    }

    sub->num_rects = i;

#ifdef DEBUG
    save_display_set(ctx);
#endif

    return 1;
}

static int dvbsub_decode(AVCodecContext *avctx,
                         void *data, int *data_size,
                         AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    DVBSubContext *ctx = avctx->priv_data;
    AVSubtitle *sub = data;
    const uint8_t *p, *p_end;
    int segment_type;
    int page_id;
    int segment_length;
    int i;

    ff_dlog(avctx, "DVB sub packet:\n");

    for (i=0; i < buf_size; i++) {
        ff_dlog(avctx, "%02x ", buf[i]);
        if (i % 16 == 15)
            ff_dlog(avctx, "\n");
    }

    if (i % 16)
        ff_dlog(avctx, "\n");

    if (buf_size <= 6 || *buf != 0x0f) {
        ff_dlog(avctx, "incomplete or broken packet");
        return AVERROR_INVALIDDATA;
    }

    p = buf;
    p_end = buf + buf_size;

    while (p_end - p >= 6 && *p == 0x0f) {
        p += 1;
        segment_type = *p++;
        page_id = AV_RB16(p);
        p += 2;
        segment_length = AV_RB16(p);
        p += 2;

        if (p_end - p < segment_length) {
            ff_dlog(avctx, "incomplete or broken packet");
            return -1;
        }

        if (page_id == ctx->composition_id || page_id == ctx->ancillary_id ||
            ctx->composition_id == -1 || ctx->ancillary_id == -1) {
            int ret = 0;
            switch (segment_type) {
            case DVBSUB_PAGE_SEGMENT:
                ret = dvbsub_parse_page_segment(avctx, p, segment_length);
                break;
            case DVBSUB_REGION_SEGMENT:
                ret = dvbsub_parse_region_segment(avctx, p, segment_length);
                break;
            case DVBSUB_CLUT_SEGMENT:
                ret = dvbsub_parse_clut_segment(avctx, p, segment_length);
                break;
            case DVBSUB_OBJECT_SEGMENT:
                ret = dvbsub_parse_object_segment(avctx, p, segment_length);
                break;
            case DVBSUB_DISPLAYDEFINITION_SEGMENT:
                ret = dvbsub_parse_display_definition_segment(avctx, p,
                                                              segment_length);
                break;
            case DVBSUB_DISPLAY_SEGMENT:
                ret = dvbsub_display_end_segment(avctx, p, segment_length, sub);
                *data_size = ret;
                break;
            default:
                ff_dlog(avctx, "Subtitling segment type 0x%x, page id %d, length %d\n",
                        segment_type, page_id, segment_length);
                break;
            }
            if (ret < 0)
                return ret;
        }

        p += segment_length;
    }

    return p - buf;
}


AVCodec ff_dvbsub_decoder = {
    .name           = "dvbsub",
    .long_name      = NULL_IF_CONFIG_SMALL("DVB subtitles"),
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_DVB_SUBTITLE,
    .priv_data_size = sizeof(DVBSubContext),
    .init           = dvbsub_init_decoder,
    .close          = dvbsub_close_decoder,
    .decode         = dvbsub_decode,
};
