    ODROID_DEBUG_PERF_START2(debug_perf_part_refr)
    int X1, XW, Line;
    int x, y, h, offset;

    uchar *PP;
    Y2++;

#if ENABLE_TRACING_GFX
    if (Y1 == 0) {
        TRACE("\n= %d =================================================\n",
            frame);
    }
    TRACE("Rendering lines %3d - %3d\tScroll: (%3d,%3d,%3d)\n",
        Y1, Y2, ScrollX, ScrollY, ScrollYDiff);
#endif

    PP = osd_gfx_buffer + XBUF_WIDTH * Y1;

    if (ScreenON && BGONSwitch) {
        y = Y1 + ScrollY - ScrollYDiff;
        offset = y & 7;
        h = 8 - offset;
        if (h > Y2 - Y1)
            h = Y2 - Y1;
        y >>= 3;
        PP -= ScrollX & 7;
        XW = io.screen_w / 8 + 1;

        /* see-through pixels show the backdrop, unless pce_clear_lines already
         * painted it (then sprites behind the background are under them) */
        const int fill = !pce_lines_cleared;
        const uchar bk = Pal[0];

        for (Line = Y1; Line < Y2; y++) {
            x = ScrollX / 8;
            y &= io.bg_h - 1;
            for (X1 = 0; X1 < XW; X1++, x++, PP += 8) {
                uchar *R, *P, *C;
                uchar *C2;
                int no, i;
                x &= io.bg_w - 1;

#if defined(WORDS_BIGENDIAN)
                no = VRAM[(x + y * io.bg_w) << 1]
                    + (VRAM[((x + y * io.bg_w) << 1) + 1] << 8);
#else
                no = ((uint16 *) VRAM)[x + y * io.bg_w];
#endif

                R = &Pal[(no >> 12) * 16];

#if ENABLE_TRACING_GFX
                // Old code was only no &= 0xFFF
                if ((no & 0xFFF) > 0x800) {
                    TRACE("GFX: Access to an invalid VRAM area "
                        "(tile pattern 0x%04x).\n", no);
                }
#endif
                no &= 0x7FF;

                if (vchange[no]) {
                    vchange[no] = 0;
                    plane2pixel(no);
                }
                C2 = (VRAM2 + (no * 8 + offset) * 4);
                C = VRAM + (no * 32 + offset * 2);
                P = PP;
                if (fill) {
                    for (i = 0; i < h; i++, P += XBUF_WIDTH, C2 += 4, C += 2) {
                        uint32 L;
                        uint16 J;
                        J = (C[0] | C[1] | C[16] | C[17]);
                        if (!J) {
                            P[0] = P[1] = P[2] = P[3] = bk;
                            P[4] = P[5] = P[6] = P[7] = bk;
                            continue;
                        }
                        L = C2[0] + (C2[1] << 8) + (C2[2] << 16)
                            + (C2[3] << 24);
                        P[0] = (J & 0x80) ? PAL((L >> 4) & 15) : bk;
                        P[1] = (J & 0x40) ? PAL((L >> 12) & 15) : bk;
                        P[2] = (J & 0x20) ? PAL((L >> 20) & 15) : bk;
                        P[3] = (J & 0x10) ? PAL((L >> 28)) : bk;
                        P[4] = (J & 0x08) ? PAL((L) & 15) : bk;
                        P[5] = (J & 0x04) ? PAL((L >> 8) & 15) : bk;
                        P[6] = (J & 0x02) ? PAL((L >> 16) & 15) : bk;
                        P[7] = (J & 0x01) ? PAL((L >> 24) & 15) : bk;
                    }
                    continue;
                }
                for (i = 0; i < h; i++, P += XBUF_WIDTH, C2 += 4, C += 2) {
                    uint32 L;
                    uint16 J;
                    J = (C[0] | C[1] | C[16] | C[17]);
                    if (!J)
                        continue;

                    L = C2[0] + (C2[1] << 8) + (C2[2] << 16)
                        + (C2[3] << 24);
                    if (J & 0x80)
                        P[0] = PAL((L >> 4) & 15);
                    if (J & 0x40)
                        P[1] = PAL((L >> 12) & 15);
                    if (J & 0x20)
                        P[2] = PAL((L >> 20) & 15);
                    if (J & 0x10)
                        P[3] = PAL((L >> 28));
                    if (J & 0x08)
                        P[4] = PAL((L) & 15);
                    if (J & 0x04)
                        P[5] = PAL((L >> 8) & 15);
                    if (J & 0x02)
                        P[6] = PAL((L >> 16) & 15);
                    if (J & 0x01)
                        P[7] = PAL((L >> 24) & 15);
                }
            }
            Line += h;
            PP += XBUF_WIDTH * h - XW * 8;
            offset = 0;
            h = Y2 - Line;
            if (h > 8)
                h = 8;
        }
    } else if (!pce_lines_cleared) {
        /* no background: the lines are the backdrop */
        pce_clear_lines(Y1, Y2);
    }
    ODROID_DEBUG_PERF_INCR2(debug_perf_part_refr, ODROID_DEBUG_PERF_SPRITE_RefreshLine)