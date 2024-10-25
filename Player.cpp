#include <iostream>
#include <SDL.h>

extern "C"
{
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/log.h"
#include "libavutil/fifo.h"
#include <libavutil/time.h>
}

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define SDL_AUDIO_BUFFER_SIZE 1024

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, VIDEO_PICTURE_QUEUE_SIZE)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

enum {
  AV_SYNC_AUDIO_MASTER,
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_MASTER,
};

static const char *input_filename;
static const char *window_title = "Media Player";

static int default_width = 800;
static int default_height = 450;
static int screen_width  = 800;
static int screen_height = 450;

static SDL_Window      *window;
static SDL_Renderer    *render;

static int is_full_screen = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static int av_sync_type = AV_SYNC_AUDIO_MASTER;

static int speed = 1;

static char error[128];

/**
 * @brief PacketQueue队列的元素,方便AVFifo初始化确认大小
 */
typedef struct _MyAVPacketList
{
    AVPacket *pkt;
} MyAVPacketList;

/**
 * @brief Packet队列
 */
typedef struct _PacketQueue
{
    AVFifo      *fifo;       //FFmpeg中一个空间很小的队列数据结构
    int         nb_packets;     //packet数量
    int         size;           //队列大小
    uint64_t    duration;  //队列时长

    SDL_mutex   *mutex;   //互斥锁
    SDL_cond    *cond;     //信号量
} PacketQueue;

typedef struct _Frame {
    AVFrame     *frame;
    double      pts;             //显示时间戳
    double      duration;      
    int64_t     pos;            /* byte position of the frame in the input file */
    int         width;
    int         height;
    int         format;
    AVRational  sar;
} Frame;

typedef struct _FrameQueue {
    Frame       queue[FRAME_QUEUE_SIZE];
    int         rindex;
    int         windex;
    int         size;
    int         abort;
    SDL_mutex   *mutex;
    SDL_cond    *cond;
} FrameQueue;

/**
 * @brief 参数结构体
 */
typedef struct _AVState
{
    //多媒体文件
    char                *filename;
    AVFormatContext     *fmt_ctx;

    //同步
    int                 av_sync_type;

    double              audio_clock; ///< the time of have audio frame
    double              frame_timer; ///< the time of have played video frame 
    double              frame_last_pts;
    double              frame_last_delay;

    double              video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    double              video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
    int64_t             video_current_pts_time;  ///< sys time (av_gettime) at which we updated video_current_pts - used to have running video pts

    //快进快退
    int                 seek_req;//[快进]/[后退]操作开启标志位
	int                 seek_flags;//[快进]/[后退]操作类型标志位
    int64_t             seek_pos;//[快进]/[后退]操作后的参考时间戳
    int                 audio_flush;
    int                 video_flush;

    //音频
    int                 audio_index;
    AVStream            *audio_st;
    AVCodecContext      *audio_ctx;
    PacketQueue         audioq;
    uint8_t             *audio_buff;
    unsigned int        audio_buff_size;
    unsigned int        audio_buff_index;
    AVFrame             *audio_frame;
    AVPacket            audio_pkt;
    uint8_t             *audio_pkt_data;
    int                 audio_pkt_size;
    struct SwrContext   *audio_swr_ctx;

    int                 audio_volume;
    //视频
    int                 video_index;
    AVStream            *video_st;
    AVCodecContext      *video_ctx;
    PacketQueue         videoq;
    AVPacket            video_pkt;
    struct SwsContext   *sws_ctx;

    SDL_Texture         *texture;

    FrameQueue          pictq;

    int width, height, xleft, ytop;
    
    SDL_Thread          *read_tid;
    SDL_Thread          *decode_tid;

    int                 quit;

}AVState;


/**
 * @brief 队列初始化
 * @param 需要初始化的队列
 * @param 0: success or AVERROR(ENOMEM)
 */
static int packet_queue_init(PacketQueue *q)
{
    //0填充
    memset(q, 0, sizeof(PacketQueue));
    //AV_FIFO_FLAG_AUTO_GROW:自动拓展标志
    q->fifo = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
    if(!q->fifo)
    {
        return AVERROR(ENOMEM);
    }

    q->mutex = SDL_CreateMutex();
    if(!q->mutex)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    q->cond = SDL_CreateCond();
    if(!q->cond)
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

/**
 * @brief 向队列放入元素,私有函数，被packet_queue_put()调用
 * @param 放入的元素
 * @return 0:success, -1 or -xx fail
 */
static int packet_queue_put_priv(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList mypkt;
    int ret = -1;

    //同步指针指向，不需要数据复制
    mypkt.pkt = pkt;

    //将mypkt写入fifo buff
    ret = av_fifo_write(q->fifo, &mypkt, 1);
    if(ret < 0) 
        return ret;

    //更新PacketQueue参数
    q->nb_packets++;
    q->size += mypkt.pkt->size + sizeof(mypkt);
    q->duration += mypkt.pkt->duration;     //队列时长

    SDL_CondSignal(q->cond);   //发送信号量通知等待的线程，现在有数据可以取了

    return 0;
}

/**
 * @brief 向队列放入元素
 * @param 目标队列
 * @param 放入元素
 * @return 0:success, -1 or -xx fail
 */
static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    //为了方便管理，内部使用自己创建的pkt1，而不是参数pkt
    AVPacket *pkt1 = nullptr;
    int ret = -1;

    pkt1 = av_packet_alloc();
    if(!pkt1)
    {
        av_packet_unref(pkt);
        return -1;
    }

    //Move every field in src to dst and reset src.
    //移动所有src的数据到dst,并且重置src packet,所以外部不需要调用av_packet_unref()进行解引用计数
    av_packet_move_ref(pkt1, pkt);

    SDL_LockMutex(q->mutex);
    //临界区上锁，确保只有一个进程在使用
    ret = packet_queue_put_priv(q, pkt1);
    SDL_UnlockMutex(q->mutex);

    if(ret < 0)
    {
        av_packet_free(&pkt1);
        return ret;
    }

    return ret;
}
/**
 * @brief 从队列中获取一个packet数据，阻塞方式和非阻塞方式
 * @param 队列
 * @param 获取的Packet存放位置
 * @param 1 阻塞方式，0 非阻塞方式
 * @return 0 success，-1 fail
 */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    MyAVPacketList mypkt;
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for(;;)
    {
        if(av_fifo_read(q->fifo, &mypkt, 1) >= 0)
        {
            q->nb_packets--;
            q->size -= mypkt.pkt->size + sizeof(mypkt) ;
            q->duration -= mypkt.pkt->duration;
            
            av_packet_move_ref(pkt, mypkt.pkt);
            av_packet_free(&mypkt.pkt);
            ret = 0;
            break;
        }
        else if(!block)
        {
            ret = -1;
            break;
        }
        else
        {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);

    return ret;
}

/**
 * @brief 清空队列
 * @param 目标队列
 */
static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList mypkt;

    if(q->fifo)
    {
        SDL_LockMutex(q->mutex);
        while(av_fifo_read(q->fifo, &mypkt, 1) >0)
        {
            av_packet_free(&mypkt.pkt);
        }

        q->nb_packets = 0;
        q->size = 0;
        q->duration = 0;

        SDL_UnlockMutex(q->mutex);
    }
    
}

/**
 * @brief 向队列中放入一个nullpacket数据
 * @param 目标队列
 * @param stream_index: stream index
 * @return 0:success, -1 or -xx fail
 */
static int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/**
 * @brief 销毁队列，内部调用packet_queue_flush和销毁 Mutex and Cond
 * @param 目标队列
 */
static void packet_queue_destroy(PacketQueue *q)
{
    if(q)
    {
        packet_queue_flush(q);
    }
    av_fifo_freep2(&q->fifo);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

/**
 * @brief frame队列初始化
 * @param 目标队列
 * @return suceess 0, -xx fail
 */
static int frame_queue_init(FrameQueue *fq)
{
    //0填充
    memset(fq, 0, sizeof(FrameQueue));

    //初始化互斥锁
    if(!(fq->mutex = SDL_CreateMutex()))
    {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex is failed: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    //初始化信号量
    if(!(fq->cond = SDL_CreateCond()))
    {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond is failed: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    //初始化frame，并没有分配内部data空间，影响不大
    for(int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
    {
        if(!(fq->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    }

    return 0;
}

/**
 * @brief 队列销毁
 * @param 目标队列
 */
static void frame_queue_destroy(FrameQueue *fq)
{
    for(int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++)
    {
        Frame *vp = &fq->queue[i];
        av_frame_unref(vp->frame);
        av_frame_free(&vp->frame);
    }
    SDL_DestroyMutex(fq->mutex);
    SDL_DestroyCond(fq->cond);
}

/**
 * @brief 队列发送信号
 * @param 目标队列
 */
static void frame_queue_signal(FrameQueue *fq)
{
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

static Frame *frame_queue_peek(FrameQueue *fq)
{
    return &fq->queue[fq->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *fq)
{
    SDL_LockMutex(fq->mutex);
    //等待,直到有空间可以放入新的frame
    while(fq->size >= VIDEO_PICTURE_QUEUE_SIZE && !fq->abort)
    {
        SDL_CondWait(fq->cond, fq->mutex);
    }
    SDL_UnlockMutex(fq->mutex);

    return &fq->queue[fq->windex];
}

//存疑
static void frame_queue_push(FrameQueue *fq)
{
    if (++fq->windex == VIDEO_PICTURE_QUEUE_SIZE)
        fq->windex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size++;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

//存疑
static void frame_queue_pop(FrameQueue *fq)
{
    Frame *vp = &fq->queue[fq->rindex];
    av_frame_unref(vp->frame);
    if (++fq->rindex == VIDEO_PICTURE_QUEUE_SIZE)
        fq->rindex = 0;
    SDL_LockMutex(fq->mutex);
    fq->size--;
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

//存疑
static void frame_queue_abort(FrameQueue *fq)
{
    SDL_LockMutex(fq->mutex);

    fq->abort = 1;

    SDL_CondSignal(fq->cond);

    SDL_UnlockMutex(fq->mutex);
}

static void stream_component_close(AVState *is, int stream_index)
{
    AVFormatContext *fmt_ctx = is->fmt_ctx;
    AVCodecParameters *codecpar;

    if(stream_index < 0 || stream_index >= fmt_ctx->nb_streams)
        return;
    codecpar = fmt_ctx->streams[stream_index]->codecpar;

    switch(codecpar->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
        {
            SDL_CloseAudio();
            swr_free(&is->audio_swr_ctx);
            av_freep(&is->audio_buff);
            is->audio_buff = nullptr;
            break;
        }
        
        //存疑
        case AVMEDIA_TYPE_VIDEO:
        {
            frame_queue_abort(&is->pictq);
            frame_queue_signal(&is->pictq);
            SDL_WaitThread(is->decode_tid, nullptr);
            is->decode_tid = nullptr;
            break;
        }

        default: break;
    }
}

static void stream_close(AVState *is)
{
    //等待read_thread退出。参数2可以设置一个int*接收线程函数返回值
    SDL_WaitThread(is->read_tid, nullptr);

    //关闭流
    if(is->video_index >= 0)
    {
        stream_component_close(is, is->video_index);
    }
    if(is->audio_index >= 0)
    {
        stream_component_close(is, is->audio_index);
    }

    avformat_close_input(&is->fmt_ctx);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);

    frame_queue_destroy(&is->pictq);

    av_free(is->filename);
    if(is->texture)
    {
        SDL_DestroyTexture(is->texture);
    }

    if(is->audio_frame)
    {
        av_frame_free(&is->audio_frame);
    }

    av_free(is);
}

static void do_exit(AVState *is)
{
    if(is)
    {
        stream_close(is);
    }
    if(render)
        SDL_DestroyRenderer(render);
    if(window)
        SDL_DestroyWindow(window);
    
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "do_exit finish!\n");
    exit(0);
}

/**
 * @brief 解码音频帧数据
 * @param 参赛结构体
 * @return suceess 返回解码的数据大小， fail 返回 -xx
 */
static int audio_decode_frame(AVState *is)
{
    AVPacket pkt;
    int ret = -1;
    int len1 = 0;
    int data_size = 0;
    char error[128];

    for(;;)
    {
        ret = packet_queue_get(&is->audioq, &pkt, 0);
        if(ret < 0)
        {
            return ret;
        }

        if(is->audio_flush)
        {
            avcodec_flush_buffers(is->audio_ctx);
            is->audio_flush = 0;
            continue;
        }
            
        ret = avcodec_send_packet(is->audio_ctx, &pkt);
        if(ret < 0)
        {
            av_strerror(ret, error, 128);
            av_log(nullptr, AV_LOG_ERROR, "avcodec_send_packet is failed, error info:%s\n", error);
            return ret;
        }

        while(ret >= 0)
        {
            ret = avcodec_receive_frame(is->audio_ctx, is->audio_frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR(EOF))
            {
                break;
            }
            else if(ret < 0)
            {
                av_strerror(ret, error, 128);
                av_log(nullptr, AV_LOG_ERROR, "avcodec_receive_frame is failed, error info:%s\n", error);
                return ret;
            }

            if(1) //!is->audio_swr_ctx && is->audio_ctx->sample_fmt != AV_SAMPLE_FMT_S16
            {
                AVChannelLayout ch_layout;
                av_channel_layout_copy(&ch_layout, &is->audio_ctx->ch_layout);
                swr_alloc_set_opts2(&is->audio_swr_ctx,
                                     &ch_layout,
                                     AV_SAMPLE_FMT_S16,
                                     is->audio_ctx->sample_rate * speed,
                                     &ch_layout,
                                     is->audio_ctx->sample_fmt,
                                     is->audio_ctx->sample_rate,
                                     0,
                                     nullptr);
                swr_init(is->audio_swr_ctx);
            }

            if(is->audio_swr_ctx)
            {
                //为了区分视频和音频，音频的frame->data使用extended_data，对于音频来说他们都是同一块地址空间
                uint8_t **in = is->audio_frame->extended_data;
                uint8_t **out = &is->audio_buff;
                int in_count = is->audio_frame->nb_samples;
                //单个通道的采样数，nb_samples+冗余值(一般为设备设置的采样个数的一半)，表示最多不可超过该采样值
                int out_count = is->audio_frame->nb_samples + 512;
               
                int out_size = av_samples_get_buffer_size(nullptr, is->audio_frame->ch_layout.nb_channels, out_count, AV_SAMPLE_FMT_S16, 0);
                //av_fast_mallo，如果地址空间已经分配会重新分配，确保audio_buff是已经分配的空间。
                av_fast_malloc(&is->audio_buff, &is->audio_buff_size, out_size);
               
                len1 = swr_convert(is->audio_swr_ctx,
                            out,                 //目标地址
                            out_count,   
                            in,      
                            in_count);
                data_size = len1 * is->audio_frame->ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
            }
            else
            {
                is->audio_buff = is->audio_frame->extended_data[0];
                data_size = av_samples_get_buffer_size(nullptr, 
                                                        is->audio_frame->ch_layout.nb_channels, 
                                                        is->audio_frame->nb_samples, 
                                                        AV_SAMPLE_FMT_S16, 0);
            }
            
            //存疑
            if (!isnan(is->audio_frame->pts))
            {
                is->audio_clock = is->audio_frame->pts + (double) is->audio_frame->nb_samples / is->audio_frame->sample_rate;
            }   
            else
            {
                is->audio_clock = NAN;
            }
            

            av_packet_unref(&pkt);
            av_frame_unref(is->audio_frame);

            return data_size;
        }
    }
    return ret;
}

/**
 * @brief SDL的音频设备播放回调函数，声卡需要数据就传送数据
 * @param 传入的参数，需要手动格式转换
 * @param SDL实际的播放缓冲区，播放的数据需要更新在该空间里
 * @param SDL播放所需要的数据长度
 */
static void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{
    int audio_size = 0;
    int len1 = 0;
    AVState *is = reinterpret_cast<AVState*>(userdata);

    while(len > 0)
    {
        //此时音频包数据已经播放完了
        if(is->audio_buff_index >= is->audio_buff_size)
        {
            //解码数据
            audio_size = audio_decode_frame(is);
            if(audio_size < 0)
            {
                is->audio_buff_size = SDL_AUDIO_BUFFER_SIZE;
                is->audio_buff = nullptr;
            }
            else
            {
                is->audio_buff_size = audio_size;
            }
            
            //成功解码出数据，"进度条"从0开始
            is->audio_buff_index = 0;
        }

        //len1当前可播放数据长度
        len1 = is->audio_buff_size - is->audio_buff_index;
        //缓冲区数据比当前扬声器所需数据多，仅截取需要部分。可以少但不能多
        if(len1 > len)
        {
            len1 = len;
        }

        //缓冲区不为空，复制需要的数据到stream
        if(is->audio_buff)
        {
            memset(stream, 0, len1);
            SDL_MixAudio(stream, (is->audio_buff + is->audio_buff_index), len1, is->audio_volume * 1.0 / 100 * 128);
            //memcpy(stream, (is->audio_buff + is->audio_buff_index), len1);
        }
        else
        {
            memset(stream, 0, len1);    //缓冲区没数据，播放静音
        }

        //更新参数
        len -= len1;
        stream += len1;
        is->audio_buff_index += len1;
    }
}

//存疑
double synchronize_video(AVState *is, AVFrame *src_frame, double pts) {

    double frame_delay;

    if(pts != 0) 
    {
        /* if we have pts, set video clock to it */
        is->video_clock = pts;
    } 
    else 
    {
        /* if we aren't given a pts, set it to the clock */
        pts = is->video_clock;
    }
    /* update the video clock */
    frame_delay = av_q2d(is->video_ctx->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    return pts;
}

static int queue_picture(AVState *is, AVFrame *src_frame, double pts, double duration, int64_t pos)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;

    //set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) 
{
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;  //传入刷新事件
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/**
 * @brief 定时刷新
 * @param 参数结构体
 * @param 定时时长
 */
static void schedule_refresh(AVState *is, int delay) 
{
    //SDL定时器回调函数
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

/**
 * @brief 视频帧解码线程
 * @param 参数结构体
 */
static int decode_thread(void *arg)
{
    int ret = -1;

    double pts, duration;

    AVState *is = reinterpret_cast<AVState*>(arg);
    AVFrame *frame = nullptr;
    Frame *vf;

    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->fmt_ctx, is->video_st, NULL);

    frame = av_frame_alloc();
    av_frame_make_writable(frame);

    //解码前需要判断退出标志
    for(;;)
    {
        if(is->quit)
        {
            break;
        }

        ret = packet_queue_get(&is->videoq, &is->video_pkt, 0);
        if(ret < 0)
        {   
            //延迟10ms重新取数据包,直到取到数据包
            av_log(is->video_ctx, AV_LOG_DEBUG, "video delay 10 ms\n");
            SDL_Delay(10);
            continue;
        }

        if(is->video_flush)
        {
            avcodec_flush_buffers(is->video_ctx);
            is->video_flush = 0;
            continue;
        }

        ret = avcodec_send_packet(is->video_ctx, &is->video_pkt);
        //可能不需要释放,后面再考虑优化
        av_packet_unref(&is->video_pkt);
        if(ret < 0)
        {
            av_log(is->video_ctx, AV_LOG_ERROR, "Failed to send pkt to video decoder!\n");
            goto __ERROR;
        }

        while(ret >= 0)
        {
            ret = avcodec_receive_frame(is->video_ctx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR(EOF))
            {
                break;
            }
            else if(ret < 0)
            {
                av_strerror(ret, error, 128);
                av_log(is->video_ctx, AV_LOG_ERROR, "Failed to receive frame from video decoder! - %s\n", error);
                goto __ERROR;
            }

            //音视频同步
            //判断帧率是否有效，效就取帧率倒数，否则为0
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            //下面代码未理解，后面补上
            pts = synchronize_video(is, frame, pts);

            //插入帧队列
            queue_picture(is, frame, pts, duration, AV_CODEC_FLAG_COPY_OPAQUE);

            av_frame_unref(frame);
        }
    }

    return 0;
    
__ERROR:
    av_frame_free(&frame);
    return ret;

}

/**
 * @brief 计算显示区域
 * @param scr_xleft,scr_ytop 区域左起点和上起点
 * @param scr_width,scr_height 区域宽高
 * @param pic_width,pic_height 图片宽高
 * @param pic_sar 宽高比
 */
static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;  //显示比例
    int64_t width, height, x, y;

    //判断显示比例，如果显示比例有问题就使用1:1；
    if(av_cmp_q(aspect_ratio, av_make_q(0,1)) <= 0)
    {
        aspect_ratio = av_make_q(1,1);
    }
    //等比例缩放 av_mul_q(a,b) = a*b
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    //存疑
    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;    //确保宽是偶数
    if (width > scr_width)  //如果宽度大于屏幕宽度就使用屏幕宽度，并重新计算高度
    {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1; //确保高是偶数
    }
    //确保显示矩形显示在屏幕中央
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);    //最小为1个像素点大小
    rect->h = FFMAX((int)height, 1);
}

/**
 * @brief 设置窗口默认大小
 * @param 图片宽度
 * @param 图片高度
 * @param 宽高比
 */
static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    int max_width  = screen_width  ? screen_width  : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w;
    default_height = rect.h;
}

/**
 * @brief 初始化音频设备参数
 * @param 参数结构体,用于音频设备回调函数
 * @return 0 success, -1 fial
 */
static int audio_open(AVState *is)
{
    int ret = -1;
    SDL_AudioSpec wanted_spec;

    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = is->audio_ctx->ch_layout.nb_channels;
    wanted_spec.freq = is->audio_ctx->sample_rate;
    wanted_spec.freq *= speed;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.silence = 0;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = reinterpret_cast<void*>(is);

    av_log(nullptr, AV_LOG_INFO, "wanted spec channel:%d, sample_rate:%d\n", wanted_spec.channels, wanted_spec.freq);
    ret = SDL_OpenAudio(&wanted_spec, nullptr);
    if(ret < 0)
    {
        SDL_Log("SDL_OpenAudio is failed\n", SDL_GetError());
        return ret;
    }

    return 0;
}

/**
 * @brief 打开流，初始化音频设备和AVState参数结构体。
 * @param 参数结构体
 * @param 流下标
 * @return suceess 0, -xxx fail
 */
static int stream_component_open(AVState *is, int stream_index)
{
    int ret = -1;

    AVFormatContext *fmt_ctx = is->fmt_ctx;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;
    AVStream *st = nullptr;

    if(stream_index < 0 || stream_index >= is->fmt_ctx->nb_streams)
    {
        return -1;
    }

    st = fmt_ctx->streams[stream_index];

    //查找解码器
    codec = avcodec_find_decoder(st->codecpar->codec_id);
    if(!codec)
    {
        av_log(nullptr, AV_LOG_FATAL, "avcodec_find_decoder is failed\n");
        return -1;
    }

    //初始化解码器上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if(!codec_ctx)
    {
        av_log(nullptr, AV_LOG_FATAL, "avcodec_find_decoder is failed\n");
        return -1;
    }

    //复制参数到解码器上下文
    ret = avcodec_parameters_to_context(codec_ctx, st->codecpar);
    if(ret < 0)
    {
        av_strerror(ret, error, 128);
        av_log(nullptr, AV_LOG_FATAL, "avcodec_parameters_to_context is failed - %s\n", error);
        goto __ERROR;
    }

    ret = avcodec_open2(codec_ctx, codec, nullptr);
    if(ret < 0)
    {
        av_strerror(ret, error, 128);
        av_log(nullptr, AV_LOG_FATAL, "avcodec_open2 is failed - %s\n", error);
        goto __ERROR;
    }

    switch(codec_ctx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
        {
            /* 为了兼容性，可能需要在上面提前设置某些参数 */
            is->audio_st = st;
            is->audio_ctx = codec_ctx;
            is->audio_buff_size = 0;
            is->audio_buff_index = 0;
            is->audio_index = stream_index;
            
            ret = audio_open(is);
            if(ret < 0)
            {
                av_log(codec_ctx, AV_LOG_FATAL, "audio_open is failed\n");
                goto __ERROR;
            }

            //0 不暂停播放，即立即打开音频设备。可以优化，后面完善。应该放在解码出音频帧数据后面。
            SDL_PauseAudio(0);
            break;
        }

        case AVMEDIA_TYPE_VIDEO:
        {
            is->video_st = st;
            is->video_index = stream_index;
            is->video_ctx = codec_ctx;
            //存疑
            is->frame_timer = (double)av_gettime() / 1000000.0; 
            is->frame_last_delay = 40e-3;   //40ms
            is->video_current_pts_time = av_gettime();

            //获取宽高比
            AVRational sar = av_guess_sample_aspect_ratio(fmt_ctx, st, nullptr);
            if(st->codecpar->width)
            {
                set_default_window_size(st->codecpar->width, st->codecpar->height, sar);
            }

            //创建视频解码线程
            is->decode_tid = SDL_CreateThread(decode_thread, "decodec_thread", is);
            break;
        }

        default:
            av_log(codec_ctx, AV_LOG_ERROR, "Unknow Codec Type: %d\n", codec_ctx->codec_type); 
            break;
    }

    //成功退出，不用avcodec_free_context(&codec_ctx);后面解码需要用到
    return ret;

__ERROR:
    if(codec_ctx)
        avcodec_free_context(&codec_ctx);
    
    return ret;

}

//读取数据线程
int read_thread(void *arg)
{
    int ret = -1;

    int video_index = -1;
    int audio_index = -1;

    AVState *is = reinterpret_cast<AVState*>(arg);
    AVFormatContext *fmt_ctx = nullptr;
    AVPacket *pkt = NULL;

    pkt = av_packet_alloc();
    if(!pkt)
    {
        av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
        goto __ERROR;
    }

    ret = avformat_open_input(&fmt_ctx, is->filename, nullptr, nullptr);
    if(ret < 0)
    {
        av_strerror(ret, error, 128);
        av_log(nullptr, AV_LOG_ERROR, "open %s is failed - %s\n", is->filename, error);
        goto __ERROR;
    }
    is->fmt_ctx = fmt_ctx;

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if(ret < 0)
    {
        av_strerror(ret, error, 128);
        av_log(nullptr, AV_LOG_FATAL, "avformat_find_stream_info is failed - %s\n", error);
        goto __ERROR;
    }

    video_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if(video_index < 0 || audio_index < 0) 
    {
        av_log(NULL, AV_LOG_ERROR, "the file must be contains audio and video stream!\n");
        goto __ERROR;
    }

    //打开音频流
    if(audio_index >= 0)
    {
        stream_component_open(is, audio_index);
    }
    //打开视频流
    if(video_index >= 0)
    {
        stream_component_open(is, video_index);
    }

    
    //循环解码
    for(;;)
    {
        //每次编码开始前都需要检测退出标志，防止卡死、延长关闭
        if(is->quit)
        {
            ret = -1;
            goto __ERROR;
        }

        //限制队列大小 < 5M
        if(is->audioq.size > MAX_QUEUE_SIZE || is->videoq.size > MAX_QUEUE_SIZE)
        {
            SDL_Delay(10);
            continue;
        }

        if(is->seek_req == 1)
        {
            if(av_seek_frame(is->fmt_ctx, -1, is->seek_pos * AV_TIME_BASE, is->seek_flags) < 0)
            {
                av_log(NULL, AV_LOG_ERROR, "av_seek_frame is failed!\n");
                is->seek_req = 0;
                continue;
            }
            else
            {
                //在执行[快进]/[快退]操作后，立刻清空缓存队列，并重置音视频解码器
                if(is->audio_index > 0)
                {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_index);
                    is->audio_flush = 1;
                }
                
                if(is->video_index > 0)
                {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put_nullpacket(&is->videoq, pkt, is->video_index);
                    is->audio_flush = 1;
                    
                }
            }
            
            is->seek_req = 0;
        }
        
        //读取解码前数据帧
        ret = av_read_frame(is->fmt_ctx, pkt);
        if(ret < 0)
        {
            //存疑，应该是为了兼容拉流因为如果解码太快到文件尾部了返回值是负数，不能直接退出
            if(is->fmt_ctx->pb->error == 0)
            {
                SDL_Delay(100); //等待用户输入
                continue;
            }
            else break;
        }

        //保存读取到的pkt
        if(pkt->stream_index == is->audio_index)
        {
            packet_queue_put(&is->audioq, pkt);            
        }
        else if(pkt->stream_index == is->video_index)
        {
            packet_queue_put(&is->videoq, pkt);
        }
        else
        {
            av_packet_unref(pkt);
        }
    }

    //解码正常结束，没有提前退出,循环等待退出
    while(!is->quit)
    {
        SDL_Delay(100);
    }

    ret = 0;

__ERROR:
    if(pkt) 
        av_packet_free(&pkt);
    
    if(ret !=0 )
    {
        SDL_Event event;
        event.type = FF_QUIT_EVENT; //发送退出事件
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
   
    return ret;
}

/**
 * @brief 打开音视频流、初始化packe队列和创建数据读取线程
 * @return 初始化好的参数结构体AVState
 */
static AVState* stream_open()
{
    AVState *is = nullptr;
    is = reinterpret_cast<AVState*>(av_mallocz(sizeof(AVState)));
    if(!is)
    {
        //AV_LOG_FATAL 致命错误
        av_log(NULL, AV_LOG_FATAL, "NO MEMORY!\n");
        return NULL;
    }

    is->audio_index = is->video_index = -1;
    is->filename = av_strdup(input_filename);
    if(!is->filename)
    {
        goto __ERROR;
    }

    is->audio_frame = av_frame_alloc();
    if(!is->audio_frame)
    {
        av_log(nullptr, AV_LOG_ERROR, "av_frame_alloc\n");
        goto __ERROR;
    }

    is->ytop    = 0;
    is->xleft   = 0;

    //设置播放音量 SDL:0 ~ 128 用户使用:0 ~ 100
    is->audio_volume = 100;

    //初始化音频、视频packet_queue
    if(packet_queue_init(&is->videoq) < 0 || packet_queue_init(&is->audioq) < 0)
    {
        goto __ERROR;
    }

    //初始化视频frame_queue
    if(frame_queue_init(&is->pictq) < 0)
    {
        goto __ERROR;
    }

    //同步方式，目前只有一种
    is->av_sync_type = av_sync_type;

    //创建读取音频视频数据线程
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) 
    {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto __ERROR;
    }

    //为显示图片设置计时器
    schedule_refresh(is, 40 / speed);

    return is;

__ERROR:
    stream_close(is);
    return NULL;
}

/**
 * @brief 通过音频时钟返回当前音频pts
 * @return 当前音频帧的pts
 */
double get_audio_clock(AVState *is) 
{
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    pts = is->audio_clock; /* maintained in the audio thread */
    hw_buf_size = is->audio_buff_size - is->audio_buff_index;   //SDL音频回调函数播放剩余数据大小
    bytes_per_sec = 0;  //每秒字节数量
    n = is->audio_ctx->ch_layout.nb_channels * 2;
    if(is->audio_st) 
    {
        bytes_per_sec = is->audio_ctx->sample_rate * n * speed;
    }
    if(bytes_per_sec) 
    {
        pts -= (double)hw_buf_size / bytes_per_sec;    //当前音频pts = 播放完该帧的pts - 播放剩余的数据时长(一般为0) 
    }

    return pts;
}

/**
 * @brief 通过获取系统时间戳和当前视频pts时间戳计算目前pts
 * @param 参数结构体
 * @return 计算的pts
 */
double get_video_clock(AVState *is) 
{
    double delta;   //增量

    delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
    return is->video_current_pts + delta;
}

/**
 * @brief 获取外部时钟，即系统时钟.
 * @param 参数可以没有
 * @return 获取到的时钟
 */
double get_external_clock(AVState *is) 
{
    return av_gettime() / 1000000.0;
}

//根据同步方式返回不同的主时钟
double get_master_clock(AVState *is) 
{
    if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) 
    {
        return get_video_clock(is);
    } 
    else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) 
    {
        return get_audio_clock(is);
    } 
    else 
    {
        return get_external_clock(is);
    }
}

static int video_open(AVState *is)
{
    int w,h;

    //设置了窗口宽高就使用设置量，否则使用默认预设值
    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;

    if(!window_title)
    {
        window_title = input_filename;
    }
    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    is->width  = w;
    is->height = h;

    return 0;
}

static void video_display(AVState *is)
{
    Frame *vf = nullptr;
    AVFrame *frame = nullptr;

    SDL_Rect rect;
    
    //1.初始化显示相关参数
    if(!is->width)
    {
        video_open(is);
    }

    //2.获取解码后的帧
    vf = frame_queue_peek(&is->pictq);

    frame = vf->frame; 

    //3.创建纹理
    if(!is->texture)
    {
        int width = frame->width;
        int height = frame->height;

        Uint32 pixformat= SDL_PIXELFORMAT_IYUV;

        is->texture = SDL_CreateTexture(render, pixformat, 
                                        SDL_TEXTUREACCESS_STREAMING,
                                        width, height);
        if(!is->texture)
        {
            av_log(nullptr, AV_LOG_ERROR, "Failed to alloct texture, NO MEMORY!\n");
            return;
        }                                
    }

    //4.计算显示矩形
    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vf->width, vf->height, vf->sar);

    //5.渲染
    SDL_UpdateYUVTexture(is->texture,
                        nullptr,
                        frame->data[0], frame->linesize[0],
                        frame->data[1], frame->linesize[1],
                        frame->data[2], frame->linesize[2]);
    SDL_RenderClear(render);
    //参数3  nullptr渲染整个纹理，而不是截取里的矩形区域，SDL_UpdateYUVTexture参数2同理
    SDL_RenderCopy(render, is->texture, nullptr, &rect);
    
    SDL_RenderPresent(render);

    //6.release frame
    frame_queue_pop(&is->pictq);
}

//音视频同步最关键的函数
static void video_refresh_timer(void *userdata)
{
    AVState *is = reinterpret_cast<AVState*>(userdata);
    Frame *vf = nullptr;

    double actual_delay, delay, sync_threshold, ref_clock, diff;

    if(is->video_st)
    {
        if(is->pictq.size == 0)
        {
            schedule_refresh(is, 1);    //快速刷新定时器,存疑
        }
        else
        {
            vf = frame_queue_peek(&is->pictq);
            is->video_current_pts = vf->pts;    //当前pts
            is->video_current_pts_time = av_gettime();  //以微妙为单位获取当前时间
            if(is->frame_last_pts == 0)
            {
                delay = 0;   //不延时，直接显示
            }
            else
            {
                delay = vf->pts - is->frame_last_pts;  //当前pts和上1帧pts差值
            }

            if(delay <= 0 || delay >= 1.0)
            {   
                //延时不正确，小于0 或者大与1s了，就使用上1帧延时
                delay = is->frame_last_delay;
            }

            //保存当前帧时间数据，为下1帧做准备
            is->frame_last_delay = delay;
            is->frame_last_pts = vf->pts;

            
            if(is->av_sync_type != AV_SYNC_VIDEO_MASTER)
            {
                ref_clock = get_master_clock(is);   //获取主时钟为参考时钟
                diff = vf->pts - ref_clock;

                //最小同步阈值0.01
                sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
                if(fabs(diff) < AV_NOSYNC_THRESHOLD)    //误差超过10秒，完辣，没救了。直接摆烂。
                {   
                    //差值为负数且小于最小阈值，(取绝对值就是大与阈值)不延迟，直接显示。
                    if(diff <= -sync_threshold) 
                    {
                        delay = 0;
                    } 
                    //差值为正且大于阈值,延时。存疑：为什么是delay = 2 * delay;
                    else if(diff >= sync_threshold) 
                    {
                        delay = 2 * delay;
                    }
                }
            }
            
            //存疑
            is->frame_timer += delay;
            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            actual_delay /= speed;
            if(actual_delay < 0.010)
            {
                //存疑
                /* Really it should skip the picture instead */
                actual_delay = 0.010;       
            }
            int delaytime = static_cast<int>(actual_delay * 1000 + 0.5);
            av_log(nullptr, AV_LOG_INFO, "delaytime = %d\n", delaytime);
            schedule_refresh(is, static_cast<int>(actual_delay * 1000  + 0.5));

            //显示图片
            video_display(is);
        }
    }
    else
    {
        //无视频流定时刷新
        schedule_refresh(is, 100);
    }
}

/**
 * SDL事件处理还需要扩展
 */
static void sdl_event_loop(AVState *is)
{
    SDL_Event event;
    for(;;)
    {
        SDL_WaitEvent(&event);
        switch(event.type)
        {
            case FF_QUIT_EVENT: break;
            case SDL_QUIT:
                is->quit = 1;
                do_exit(is);
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            case SDL_WINDOWEVENT:
                SDL_GetWindowSize(window, &is->width, &is->height);
                SDL_Log("SDL_WINDOWEVENT win_width:%d, win_height:%d\n", is->width, is->height);
                break;
            case SDL_KEYDOWN: 
                {
                    if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) 
                    {
                        is->quit = 1;
                        do_exit(is);
                        break;
                    }

                    if(event.key.keysym.sym == SDLK_DOWN)
                    {
                        is->audio_volume -= 10;
                        if(is->audio_volume < 0)
                        {
                            is->audio_volume = 0;
                        }
                        SDL_Log("volume down: %d\n", is->audio_volume);
                        break;
                    }

                    if(event.key.keysym.sym == SDLK_UP)
                    {
                        is->audio_volume += 10;
                        if(is->audio_volume > 100)
                        {
                            is->audio_volume = 100;
                        }
                        SDL_Log("volume up: %d\n", is->audio_volume);
                        break;
                    }

                    if(event.key.keysym.sym == SDLK_LEFT)
                    {
                        is->seek_req = 1;
                        is->seek_flags = AVSEEK_FLAG_BACKWARD;
                        is->seek_pos = is->frame_last_pts - 5.0;
                        if(is->seek_pos < 0)
                        {
                            is->seek_pos = 0;
                        }
                        SDL_Log("seek left: %d\n", is->seek_pos);
                        break;
                    }
                    
                    if(event.key.keysym.sym == SDLK_RIGHT)
                    {
                        is->seek_req = 1;
                        is->seek_flags = AVSEEK_FLAG_BACKWARD;
                        is->seek_pos = is->frame_last_pts + 5.0;
                        SDL_Log("seek right: %d\n", is->seek_pos);
                       break;
                    }
                }
            default:
                break;
        }
    }
}

int main(int argc, char** argv)
{
    int flags = 0;
    AVState *is = nullptr;

    av_log_set_level(AV_LOG_ERROR);

    if(argc < 3)
    {
        av_log(nullptr, AV_LOG_ERROR, "Usage: command <file> speed\n");
        exit(-1);
    }

    input_filename = argv[1];
    speed = atof(argv[2]);

    flags = SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER;
    if(SDL_Init(flags))
    {
        av_log(nullptr, AV_LOG_ERROR, "Could not inti SDL - %s\n", SDL_GetError());
        exit(-1);
    }

    window = SDL_CreateWindow(window_title, 
                             SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED,
                             default_width, default_height,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if(window)
    {
        //SDL_RENDERER_ACCELERATED 使用硬件加速
        //
        render = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    }

    if(!window || !render)
    {
        av_log(nullptr, AV_LOG_ERROR, "Create Window or Renderer fail!\n");
        do_exit(nullptr);
    }

    //打开音频流和视频流
    is = stream_open();
    if(!is)
    {
        av_log(nullptr, AV_LOG_ERROR, "Failed to initialize AVState\n");
        do_exit(nullptr);
    }

    //SDL事件循环处理
    sdl_event_loop(is);

    return 0;
}