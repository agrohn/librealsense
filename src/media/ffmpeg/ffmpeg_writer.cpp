#include "ffmpeg_writer.h"
#include <iostream>
using namespace std;
#define STREAM_DURATION 10.0
#define DEFAULT_PIX_FMT AV_PIX_FMT_YUV420P
#define STREAM_FRAME_RATE 30
#define DEFAULT_VIDEO_CODEC "h264_nvenc"
#define DEFAULT_AUDIO_CODEC "aac"

using OutputStream = librealsense::ffmpeg_writer::OutputStream;
using namespace librealsense;
void
librealsense::ffmpeg_writer::close_stream(AVFormatContext *oc, OutputStream *ost)
{
  avcodec_free_context(&ost->enc);
  av_frame_free(&ost->frame);
  av_frame_free(&ost->tmp_frame);
  if ( ost->sws_ctx != nullptr ) sws_freeContext(ost->sws_ctx);
  if ( ost->swr_ctx != nullptr ) swr_free(&ost->swr_ctx);
}
static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {

        throw unrecoverable_exception("Could not allocate frame data", RS2_EXCEPTION_TYPE_UNKNOWN);
    }

    return picture;
}
// just a test at this stage for compiling.
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static AVFrame *make_video_frame(OutputStream *ost, const uint8_t *rgbdata)
{
    AVCodecContext *c = ost->enc;

    /* check if we want to generate more frames */
    if (av_compare_ts(ost->next_pts, c->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    if ( !ost->sws_ctx )
    {
      // converts from RGB to YUV420P
      ost->sws_ctx  = sws_getContext(c->width, c->height, AV_PIX_FMT_RGB24,
                                     c->width, c->height, AV_PIX_FMT_YUV420P,
                                     0, 0, 0, 0);
    }
    //uint8_t * rgbdata = new uint8_t[c->width* c->height* 3];
    //create_rgb_frame(rgbdata, ost->next_pts, c->width, c->height);
    const uint8_t * inData[1] = { rgbdata }; // RGB24 have one plane
    int inLinesize[1] = { 3*c->width }; // RGB stride
    sws_scale(ost->sws_ctx, inData, inLinesize, 0, c->height, ost->frame->data, ost->frame->linesize);

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}
static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    //log_packet(fmt_ctx, pkt);
    return av_interleaved_write_frame(fmt_ctx, pkt);
}
static int write_video_frame_ffmpeg(AVFormatContext *oc, OutputStream *ost, const uint8_t *rgbdata)
{
    int ret;
    AVCodecContext *c;
    AVFrame *frame;
    int got_packet = 0;
    AVPacket pkt = { 0 };

    c = ost->enc;
    frame = make_video_frame(ost, rgbdata);

    av_init_packet(&pkt);
    c->pix_fmt = DEFAULT_PIX_FMT;
    /* encode the image */
    ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
    if (ret < 0) {
        fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    if (got_packet) {
        ret = write_frame(oc, &c->time_base, ost->st, &pkt);
    } else {
        cerr << "did not get packet, not writing frame or anything\n";
        ret = 0;
    }

    if (ret < 0) {
        fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    return (frame || got_packet) ? 0 : 1;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);
    c->pix_fmt = DEFAULT_PIX_FMT;
    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }
    assert( c->pix_fmt == DEFAULT_PIX_FMT);
    /* allocate and init a re-usable frame */
    ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
      ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
      if (!ost->tmp_frame) {
        fprintf(stderr, "Could not allocate temporary picture\n");
        exit(1);
      }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

void
librealsense::ffmpeg_writer::add_stream_open_video()
{

  add_stream(&video_st, oc, &video_codec, DEFAULT_VIDEO_CODEC);
  
  if ( ret = avio_open(&oc->pb, m_file_path.c_str(), AVIO_FLAG_WRITE) < 0 )
  {
    std::stringstream ss;
    ss << "Could not open file, " << av_err2str(ret);
    throw io_exception(ss.str());
  }

  open_video(oc, video_codec, &video_st, opt);
  av_dump_format(oc, 0, m_file_path.c_str(), 1);
  
  /* open the output file, if needed */
  if (!(fmt->flags & AVFMT_NOFILE)) {
    ret = avio_open(&oc->pb, m_file_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {

      stringstream ss;
      ss << "Could not open '" << m_file_path << "', " << av_err2str(ret);
      throw unrecoverable_exception(ss.str(), RS2_EXCEPTION_TYPE_UNKNOWN);
    }
  }
  
  /* Write the stream header, if any. */
  ret = avformat_write_header(oc, &opt);
  if (ret < 0) {
    stringstream ss;
    ss << "Error occurred when opening output file: '" << m_file_path << "', " << av_err2str(ret);
    throw unrecoverable_exception(ss.str(), RS2_EXCEPTION_TYPE_UNKNOWN);
  }
  
}
void
librealsense::ffmpeg_writer::add_stream(OutputStream *ost, AVFormatContext *oc,
                             AVCodec **codec,
                             const char * codec_name)
{
  AVCodecContext *c;
  int i;
  /* find the encoder */
  *codec = avcodec_find_encoder_by_name(codec_name);
  //*codec = avcodec_find_encoder(codec_id);
  if (!(*codec))
  {
    std::stringstream ss;
    ss << "Could not find encoder for '" << codec_name << "'";
    throw unrecoverable_exception(ss.str(), RS2_EXCEPTION_TYPE_UNKNOWN);
  }
  
  ost->st = avformat_new_stream(oc, NULL);
  if (!ost->st)
    throw unrecoverable_exception("Could not allocate stream", RS2_EXCEPTION_TYPE_UNKNOWN);

  ost->st->id = oc->nb_streams-1;
  c = avcodec_alloc_context3(*codec);
  c->pix_fmt = AV_PIX_FMT_YUV420P;
  
  if (!c)
    throw unrecoverable_exception("Could not alloc an encoding context", RS2_EXCEPTION_TYPE_UNKNOWN);

  ost->enc = c;
  
  switch ((*codec)->type) {
    /*  case AVMEDIA_TYPE_AUDIO:
    c->sample_fmt  = (*codec)->sample_fmts ?
    (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = 64000;
    c->sample_rate = 44100;
    if ((*codec)->supported_samplerates) {
      c->sample_rate = (*codec)->supported_samplerates[0];
      for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == 44100)
                    c->sample_rate = 44100;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = AV_CH_LAYOUT_STEREO;
        if ((*codec)->channel_layouts) {
            c->channel_layout = (*codec)->channel_layouts[0];
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
        ost->st->time_base = (AVRational){ 1, c->sample_rate };
        break;
    */
    case AVMEDIA_TYPE_VIDEO:
      //c->codec_id = codec_id;
      
      /*
        cout << "supported pixel formats:\n";
        for (int i = 0; (*codec)->pix_fmts[i] != AV_PIX_FMT_NONE; i++)
        cout << av_get_pix_fmt_name((*codec)->pix_fmts[i]) << "\n";
      */
      
      c->bit_rate = 400000;
        /* Resolution must be a multiple of two. */
      c->width    = video_width;
      c->height   = video_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
        c->time_base       = ost->st->time_base;

        c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = DEFAULT_PIX_FMT;
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
    break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

void 
librealsense::ffmpeg_writer::write_video_frame(const stream_identifier& stream_id, const nanoseconds& timestamp, frame_holder&& frame)
{
  if ( video_codec == nullptr )
  {
    add_stream_open_video();
  }
  
  auto vid_frame = dynamic_cast<librealsense::video_frame*>(frame.frame);
  auto depth_frame = dynamic_cast<librealsense::depth_frame*>(frame.frame);
  bool isDepth = (depth_frame != nullptr);
  assert(vid_frame != nullptr);
  
  if ( isDepth ) return;
  
  std::chrono::duration<double, std::milli> timestamp_ms(vid_frame->get_frame_timestamp());
  cerr << __FUNCTION__ << " writing " << (isDepth ? "depth" : "video") << " frame " << timestamp_ms.count() << "\n";
  
  //image.width = static_cast<uint32_t>(vid_frame->get_width());
  //image.height = static_cast<uint32_t>(vid_frame->get_height());
  //image.step = static_cast<uint32_t>(vid_frame->get_stride());
  //convert(vid_frame->get_stream()->get_format(), image.encoding);
  //image.is_bigendian = is_big_endian();
  auto size = vid_frame->get_stride() * vid_frame->get_height();
  auto p_data = vid_frame->get_frame_data();
  

  //image.data.assign(p_data, p_data + size);
  //image.header.seq = static_cast<uint32_t>(vid_frame->get_frame_number());

  //image.header.stamp = rs2rosinternal::Time(std::chrono::duration<double>(timestamp_ms).count());
  //std::string TODO_CORRECT_ME = "0";
  //image.header.frame_id = TODO_CORRECT_ME;
  //auto image_topic = ros_topic::frame_data_topic(stream_id);
  //write_message(image_topic, timestamp, image);
  //write_additional_frame_messages(stream_id, timestamp, frame);

  write_video_frame_ffmpeg(oc, &video_st, p_data);
}
