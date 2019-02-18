// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved.

#pragma once

#include <string>
#include <memory>
#include <iomanip>
#include <ios>      //For std::hexfloat
#include "core/debug.h"
#include "core/serialization.h"
#include "archive.h"
#include "types.h"
#include "stream.h"
#include <iostream>
#include <sstream>  // for error message construction
extern "C" {
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/pixdesc.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
// fixes for cpp compiler 
#undef av_err2str
#define av_err2str(errnum)                                              \
  av_make_error_string((char*)__builtin_alloca(AV_ERROR_MAX_STRING_SIZE), AV_ERROR_MAX_STRING_SIZE, errnum) 
#undef av_ts2timestr
#define av_ts2timestr(ts, tb)                                           \
  av_ts_make_time_string((char*)__builtin_alloca(AV_TS_MAX_STRING_SIZE), ts, tb)

#undef av_ts2timestr
#define av_ts2timestr(ts, tb)                                           \
  av_ts_make_time_string((char*)__builtin_alloca(AV_TS_MAX_STRING_SIZE), ts, tb)

#undef av_ts2str
#define av_ts2str(ts) av_ts_make_string((char*)__builtin_alloca(AV_TS_MAX_STRING_SIZE), ts)

namespace librealsense
{
    using namespace device_serializer;

    class ffmpeg_writer: public writer
    {
    public:

      // a wrapper around a single output AVStream
      typedef struct OutputStream {
        AVStream *st{nullptr};
        AVCodecContext *enc{nullptr};
        
        /* pts of the next frame that will be generated */
        int64_t next_pts{0};
        int samples_count{0};
        
        AVFrame *frame{nullptr};
        AVFrame *tmp_frame{nullptr};
        
        float t, tincr, tincr2;
        
        struct SwsContext *sws_ctx={nullptr};
        struct SwrContext *swr_ctx={nullptr};
      } OutputStream;
    private:
      OutputStream video_st;
      AVOutputFormat *fmt{nullptr};
      AVFormatContext *oc{nullptr};
      AVCodec *video_codec{nullptr};
      int ret;
      AVDictionary *opt = NULL;
      size_t video_width{0};
      size_t video_height{0};
      
    private:
      void add_stream(OutputStream *ost, AVFormatContext *oc,
                      AVCodec **codec,
                      const char * codec_name);
      
      void close_stream(AVFormatContext *oc, OutputStream *ost);

    public:
        explicit ffmpeg_writer(const std::string& file) : m_file_path(file)
        {
          std::cerr << __FUNCTION__
                    << " : opening ffmpeg_writer for file " << m_file_path << "\n";
          av_register_all();

          int ret = 0;

          avformat_alloc_output_context2(&oc, NULL, NULL, file.c_str());
          if (!oc)
          {
            std::stringstream ss;
            ss << "Could not deduce output format from file extension from '" << file << "'";
            throw unrecoverable_exception(ss.str(),RS2_EXCEPTION_TYPE_UNKNOWN);
          }

          fmt = oc->oformat;

          if (fmt->video_codec == AV_CODEC_ID_NONE)
          {
            throw unrecoverable_exception("Format has no video codec, cannot encode stream!",
                                          RS2_EXCEPTION_TYPE_UNKNOWN);
          }
          



          
          
        }
        virtual ~ffmpeg_writer()
        {
          if ( oc )
          {
            close_stream(oc, &video_st);
            av_write_trailer(oc);
            // close output file
            avio_closep(&oc->pb);
            avformat_free_context(oc);
          }
        }
      void add_stream_open_video();
      
        void write_device_description(const librealsense::device_snapshot& device_description) override
        {
          std::cerr << __FUNCTION__ << " : writing device description\n";
          /*for (auto&& device_extension_snapshot : device_description.get_device_extensions_snapshots().get_snapshots())
          {
            write_extension_snapshot(get_device_index(), get_static_file_info_timestamp(), device_extension_snapshot.first, device_extension_snapshot.second);


          }*/
          
          for (auto&& sensors_snapshot : device_description.get_sensors_snapshots())
          {
            for (auto&& sensor_extension_snapshot : sensors_snapshot.get_sensor_extensions_snapshots().get_snapshots())
            {
              rs2_extension type = sensor_extension_snapshot.first;
              std::cerr << "extension was: " << rs2_extension_to_string(type) << "\n";
              if (type == RS2_EXTENSION_VIDEO_PROFILE)
              {
                
                auto profile = SnapshotAs<RS2_EXTENSION_VIDEO_PROFILE>(sensor_extension_snapshot.second);
                std::cerr << "video stream dimensions: "
                          << profile->get_width() << " x "
                          << profile->get_height() << "\n";
                
              }


            }
          }
        }

        void write_frame(const stream_identifier& stream_id, const nanoseconds& timestamp, frame_holder&& frame) 
        {
            if (Is<video_frame>(frame.frame))
            {
                write_video_frame(stream_id, timestamp, std::move(frame));
                return;
            }

            /*if (Is<motion_frame>(frame.frame))
            {
                write_motion_frame(stream_id, timestamp, std::move(frame));
                return;
                }*/

            /*if (Is<pose_frame>(frame.frame))
            {
                write_pose_frame(stream_id, timestamp, std::move(frame));
                return;
                }*/
        }

        void write_snapshot(uint32_t device_index, const nanoseconds& timestamp, rs2_extension type, const std::shared_ptr<extension_snapshot>& snapshot) override
        {
          std::cerr << __FUNCTION__ << "IGNORED\n";
          std::cerr << "extension was: " << rs2_extension_to_string(type) << "\n";
          //write_extension_snapshot(device_index, -1, timestamp, type, snapshot);
        }
      
        void write_snapshot(const sensor_identifier& sensor_id, const nanoseconds& timestamp, rs2_extension type, const std::shared_ptr<extension_snapshot>& snapshot) override
        { 
          //std::cerr << __FUNCTION__ << "IGNORED 2\n";
          std::cerr << "extension was: " << rs2_extension_to_string(type) << "\n";
          if (type == RS2_EXTENSION_VIDEO_PROFILE)
          {
            
            auto profile = SnapshotAs<RS2_EXTENSION_VIDEO_PROFILE>(snapshot);
            std::cerr << "video stream dimensions: "
                      << profile->get_width() << " x "
                      << profile->get_height() << "\n";

            if ( (video_width > 0 && video_width != profile->get_width() ) ||
                 (video_height > 0 && video_height != profile->get_height()) )
            {
              throw unrecoverable_exception("Video and Depth dimensions do not match, cannot use encode video with ffmpeg!",RS2_EXCEPTION_TYPE_UNKNOWN);
            }
            video_width = profile->get_width();
            video_height = profile->get_height();
          }
          // write_extension_snapshot(sensor_id.device_index, sensor_id.sensor_index, timestamp, type, snapshot);
        }

      
        void write_notification(const sensor_identifier& sensor_id, const nanoseconds& timestamp, const notification& n)
        {
          std::cerr << "skipping write_notification\n"; 
          /* realsense_msgs::Notification noti_msg = to_notification_msg(n);
             write_message(ros_topic::notification_topic({ sensor_id.device_index, sensor_id.sensor_index}, n.category), timestamp, noti_msg);*/
        }

        const std::string& get_file_name() const override
        {
            return m_file_path;
        }
      
    private:
        void write_file_version()
        {
          std::cerr << __FUNCTION__ << "\n";
          /*std_msgs::UInt32 msg;
            msg.data = get_file_version();
            write_message(ros_topic::file_version_topic(), get_static_file_info_timestamp(), msg);*/
        }


        void write_frame_metadata(const stream_identifier& stream_id, const nanoseconds& timestamp, frame_interface* frame)
        {
          std::cerr << "ignoring frame metadata\n";
         /*auto metadata_topic = ros_topic::frame_metadata_topic(stream_id);
            diagnostic_msgs::KeyValue system_time;
            system_time.key = SYSTEM_TIME_MD_STR;
            system_time.value = std::to_string(frame->get_frame_system_time());
            write_message(metadata_topic, timestamp, system_time);

            diagnostic_msgs::KeyValue timestamp_domain;
            timestamp_domain.key = TIMESTAMP_DOMAIN_MD_STR;
            timestamp_domain.value = librealsense::get_string(frame->get_frame_timestamp_domain());
            write_message(metadata_topic, timestamp, timestamp_domain);

            for (int i = 0; i < static_cast<rs2_frame_metadata_value>(rs2_frame_metadata_value::RS2_FRAME_METADATA_COUNT); i++)
            {
                rs2_frame_metadata_value type = static_cast<rs2_frame_metadata_value>(i);
                if (frame->supports_frame_metadata(type))
                {
                    auto md = frame->get_frame_metadata(type);
                    diagnostic_msgs::KeyValue md_msg;
                    md_msg.key = librealsense::get_string(type);
                    md_msg.value = std::to_string(md);
                    write_message(metadata_topic, timestamp, md_msg);
                }
                }*/
        }

        void write_extrinsics(const stream_identifier& stream_id, frame_interface* frame)
        {
          std::cerr << "write_extrinsics\n";
          /*  if (m_extrinsics_msgs.find(stream_id) != m_extrinsics_msgs.end())
            {
                return; //already wrote it
                }
            auto& dev = frame->get_sensor()->get_device();
            uint32_t reference_id = 0;
            rs2_extrinsics ext;
            std::tie(reference_id, ext) = dev.get_extrinsics(*frame->get_stream());
            geometry_msgs::Transform tf_msg;
            convert(ext, tf_msg);
            write_message(ros_topic::stream_extrinsic_topic(stream_id, reference_id), get_static_file_info_timestamp(), tf_msg);
            m_extrinsics_msgs[stream_id] = tf_msg;*/
        }

      /*realsense_msgs::Notification to_notification_msg(const notification& n)
        {
            realsense_msgs::Notification noti_msg;
            noti_msg.category = get_string(n.category);
            noti_msg.severity = get_string(n.severity);
            noti_msg.description = n.description;
            auto secs = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::duration<double, std::nano>(n.timestamp));
            noti_msg.timestamp = rs2rosinternal::Time(secs.count());
            noti_msg.serialized_data = n.serialized_data;
            return noti_msg;
            }*/

      

        void write_additional_frame_messages(const stream_identifier& stream_id, const nanoseconds& timestamp, frame_interface* frame)
        {
          std::cerr << "skipping write_additional_frame_messages\n";
          /*try
            {
                write_frame_metadata(stream_id, timestamp, frame);
            }
            catch (std::exception const& e)
            {
                LOG_WARNING("Failed to write frame metadata for " << stream_id << ". Exception: " << e.what());
            }

            try
            {
                write_extrinsics(stream_id, frame);
            }
            catch (std::exception const& e)
            {
                LOG_WARNING("Failed to write stream extrinsics for " << stream_id << ". Exception: " << e.what());
                }*/
        }
        
      void write_video_frame(const stream_identifier& stream_id, const nanoseconds& timestamp, frame_holder&& frame);
         

        void write_stream_info(nanoseconds timestamp, const sensor_identifier& sensor_id, std::shared_ptr<stream_profile_interface> profile)
        {
          /*realsense_msgs::StreamInfo stream_info_msg;
            stream_info_msg.is_recommended = profile->get_tag() & profile_tag::PROFILE_TAG_DEFAULT;
            convert(profile->get_format(), stream_info_msg.encoding);
            stream_info_msg.fps = profile->get_framerate();
            write_message(ros_topic::stream_info_topic({ sensor_id.device_index, sensor_id.sensor_index, profile->get_stream_type(), static_cast<uint32_t>(profile->get_stream_index()) }), timestamp, stream_info_msg);*/
        }

        void write_streaming_info(nanoseconds timestamp, const sensor_identifier& sensor_id, std::shared_ptr<video_stream_profile_interface> profile)
        {
          //write_stream_info(timestamp, sensor_id, profile);
            
          /* sensor_msgs::CameraInfo camera_info_msg;
            camera_info_msg.width = profile->get_width();
            camera_info_msg.height = profile->get_height();
            rs2_intrinsics intrinsics{};
            try {
                intrinsics = profile->get_intrinsics();
            }
            catch (...)
            {
                LOG_ERROR("Error trying to get intrinsc data for stream " << profile->get_stream_type() << ", " << profile->get_stream_index());
            }
            camera_info_msg.K[0] = intrinsics.fx;
            camera_info_msg.K[2] = intrinsics.ppx;
            camera_info_msg.K[4] = intrinsics.fy;
            camera_info_msg.K[5] = intrinsics.ppy;
            camera_info_msg.K[8] = 1;
            camera_info_msg.D.assign(std::begin(intrinsics.coeffs), std::end(intrinsics.coeffs));
            camera_info_msg.distortion_model = rs2_distortion_to_string(intrinsics.model);
            write_message(ros_topic::video_stream_info_topic({ sensor_id.device_index, sensor_id.sensor_index, profile->get_stream_type(), static_cast<uint32_t>(profile->get_stream_index()) }), timestamp, camera_info_msg);*/
        }

        

        template <rs2_extension E>
        std::shared_ptr<typename ExtensionToType<E>::type> SnapshotAs(std::shared_ptr<librealsense::extension_snapshot> snapshot)
        {
            auto as_type = As<typename ExtensionToType<E>::type>(snapshot);
            if (as_type == nullptr)
            {
                throw invalid_value_exception(to_string() << "Failed to cast snapshot to \"" << E << "\" (as \"" << ExtensionToType<E>::to_string() << "\")");
            }
            return as_type;
        }


        template <typename T>
        void write_message(std::string const& topic, nanoseconds const& time, T const& msg)
        {
          std::cerr << "Write Message - recording\n";
          /*try
            {
                m_bag.write(topic, to_rostime(time), msg);
                LOG_DEBUG("Recorded: \"" << topic << "\" . TS: " << time.count());
            }
            catch (rosbag::BagIOException& e)
            {
                throw io_exception(to_string() << "Ros Writer failed to write topic: \"" << topic << "\" to file. (Exception message: " << e.what() << ")");
                }*/
        }

        static uint8_t is_big_endian()
        {
            int num = 1;
            return (*reinterpret_cast<char*>(&num) == 1) ? 0 : 1; //Little Endian: (char)0x0001 => 0x01, Big Endian: (char)0x0001 => 0x00,
        }


        std::string m_file_path;


    };
}
