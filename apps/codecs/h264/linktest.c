/* Link check for libffh264.a: the calls a codec module makes. Never run. */
#include "libavcodec/avcodec.h"

int decode_test(const uint8_t *data, int size);

int decode_test(const uint8_t *data, int size)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return -1;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return -2;
    ctx->thread_count = 2;
    ctx->thread_type = FF_THREAD_FRAME;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return -3;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    pkt->data = (uint8_t *)data;
    pkt->size = size;
    int frames = 0;
    if (avcodec_send_packet(ctx, pkt) >= 0)
        while (avcodec_receive_frame(ctx, frame) >= 0)
            frames++;
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return frames;
}

int main(void)
{
    return decode_test((const uint8_t *)0, 0);
}
