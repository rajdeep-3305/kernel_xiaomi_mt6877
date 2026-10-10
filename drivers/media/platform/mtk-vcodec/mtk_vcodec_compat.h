#ifndef _MTK_VCODEC_COMPAT_H_
#define _MTK_VCODEC_COMPAT_H_

#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <media/v4l2-mem2mem.h>

#ifndef V4L2_CTRL_CLASS_MPEG
#define V4L2_CTRL_CLASS_MPEG V4L2_CTRL_CLASS_CODEC
#endif

#ifndef V4L2_PIX_FMT_ARGB1010102
#define V4L2_PIX_FMT_ARGB1010102 v4l2_fourcc('A', 'B', '3', '0')
#endif
#ifndef V4L2_PIX_FMT_ABGR1010102
#define V4L2_PIX_FMT_ABGR1010102 v4l2_fourcc('A', 'R', '3', '0')
#endif
#ifndef V4L2_PIX_FMT_BGRA1010102
#define V4L2_PIX_FMT_BGRA1010102 v4l2_fourcc('B', 'A', '3', '0')
#endif
#ifndef V4L2_PIX_FMT_P010M
#define V4L2_PIX_FMT_P010M v4l2_fourcc('P', '0', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_H265
#define V4L2_PIX_FMT_H265 v4l2_fourcc('H', '2', '6', '5')
#endif
#ifndef V4L2_PIX_FMT_HEIF
#define V4L2_PIX_FMT_HEIF v4l2_fourcc('H', 'E', 'I', 'F')
#endif
#ifndef V4L2_PIX_FMT_WMV1
#define V4L2_PIX_FMT_WMV1 v4l2_fourcc('W', 'M', 'V', '1')
#endif
#ifndef V4L2_PIX_FMT_WMV2
#define V4L2_PIX_FMT_WMV2 v4l2_fourcc('W', 'M', 'V', '2')
#endif
#ifndef V4L2_PIX_FMT_WMV3
#define V4L2_PIX_FMT_WMV3 v4l2_fourcc('W', 'M', 'V', '3')
#endif
#ifndef V4L2_PIX_FMT_WMVA
#define V4L2_PIX_FMT_WMVA v4l2_fourcc('W', 'M', 'V', 'A')
#endif
#ifndef V4L2_PIX_FMT_WVC1
#define V4L2_PIX_FMT_WVC1 v4l2_fourcc('W', 'V', 'C', '1')
#endif
#ifndef V4L2_PIX_FMT_AV1
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_MT10S
#define V4L2_PIX_FMT_MT10S v4l2_fourcc('M', '1', '0', 'S')
#endif
#ifndef V4L2_PIX_FMT_MT10
#define V4L2_PIX_FMT_MT10 v4l2_fourcc('M', 'T', '1', '0')
#endif
#ifndef V4L2_PIX_FMT_P010S
#define V4L2_PIX_FMT_P010S v4l2_fourcc('P', '0', '1', 'S')
#endif
#ifndef V4L2_PIX_FMT_RGB32_AFBC
#define V4L2_PIX_FMT_RGB32_AFBC v4l2_fourcc('M', 'C', 'R', '8')
#endif
#ifndef V4L2_PIX_FMT_BGR32_AFBC
#define V4L2_PIX_FMT_BGR32_AFBC v4l2_fourcc('M', 'C', 'B', '8')
#endif
#ifndef V4L2_PIX_FMT_RGBA1010102_AFBC
#define V4L2_PIX_FMT_RGBA1010102_AFBC v4l2_fourcc('M', 'C', 'R', 'X')
#endif
#ifndef V4L2_PIX_FMT_BGRA1010102_AFBC
#define V4L2_PIX_FMT_BGRA1010102_AFBC v4l2_fourcc('M', 'C', 'B', 'X')
#endif
#ifndef V4L2_PIX_FMT_NV12_AFBC
#define V4L2_PIX_FMT_NV12_AFBC v4l2_fourcc('M', 'C', 'N', '8')
#endif
#ifndef V4L2_PIX_FMT_NV12_10B_AFBC
#define V4L2_PIX_FMT_NV12_10B_AFBC v4l2_fourcc('M', 'C', 'N', 'X')
#endif

#ifndef V4L2_BUF_FLAG_REF_FREED
#define V4L2_BUF_FLAG_REF_FREED 0x00000200
#endif
#ifndef V4L2_BUF_FLAG_CROP_CHANGED
#define V4L2_BUF_FLAG_CROP_CHANGED 0x00008000
#endif
#ifndef V4L2_BUF_FLAG_ROI
#define V4L2_BUF_FLAG_ROI 0x00400000
#endif
#ifndef V4L2_BUF_FLAG_HDR_META
#define V4L2_BUF_FLAG_HDR_META 0x01000000
#endif
#ifndef V4L2_BUF_FLAG_QP_META
#define V4L2_BUF_FLAG_QP_META 0x02000000
#endif

#ifndef V4L2_EVENT_MTK_VCODEC_START
#define V4L2_EVENT_MTK_VCODEC_START (V4L2_EVENT_PRIVATE_START + 0x00002000)
#define V4L2_EVENT_MTK_VDEC_ERROR (V4L2_EVENT_MTK_VCODEC_START + 1)
#define V4L2_EVENT_MTK_VDEC_NOHEADER (V4L2_EVENT_MTK_VCODEC_START + 2)
#define V4L2_EVENT_MTK_VENC_ERROR (V4L2_EVENT_MTK_VCODEC_START + 3)
#endif

#ifndef V4L2_CID_MPEG_MTK_BASE
#define V4L2_CID_MPEG_MTK_BASE (V4L2_CTRL_CLASS_MPEG | 0x2000)
#define V4L2_CID_MPEG_MTK_FRAME_INTERVAL (V4L2_CID_MPEG_MTK_BASE + 0)
#define V4L2_CID_MPEG_MTK_DECODE_MODE (V4L2_CID_MPEG_MTK_BASE + 2)
#define V4L2_CID_MPEG_MTK_FRAME_SIZE (V4L2_CID_MPEG_MTK_BASE + 3)
#define V4L2_CID_MPEG_MTK_FIXED_MAX_FRAME_BUFFER (V4L2_CID_MPEG_MTK_BASE + 4)
#define V4L2_CID_MPEG_MTK_CRC_PATH (V4L2_CID_MPEG_MTK_BASE + 5)
#define V4L2_CID_MPEG_MTK_GOLDEN_PATH (V4L2_CID_MPEG_MTK_BASE + 6)
#define V4L2_CID_MPEG_MTK_COLOR_DESC (V4L2_CID_MPEG_MTK_BASE + 7)
#define V4L2_CID_MPEG_MTK_ASPECT_RATIO (V4L2_CID_MPEG_MTK_BASE + 8)
#define V4L2_CID_MPEG_MTK_SET_WAIT_KEY_FRAME (V4L2_CID_MPEG_MTK_BASE + 9)
#define V4L2_CID_MPEG_MTK_SET_NAL_SIZE_LENGTH (V4L2_CID_MPEG_MTK_BASE + 10)
#define V4L2_CID_MPEG_MTK_SEC_DECODE (V4L2_CID_MPEG_MTK_BASE + 11)
#define V4L2_CID_MPEG_MTK_FIX_BUFFERS (V4L2_CID_MPEG_MTK_BASE + 12)
#define V4L2_CID_MPEG_MTK_FIX_BUFFERS_SVP (V4L2_CID_MPEG_MTK_BASE + 13)
#define V4L2_CID_MPEG_MTK_INTERLACING (V4L2_CID_MPEG_MTK_BASE + 14)
#define V4L2_CID_MPEG_MTK_CODEC_TYPE (V4L2_CID_MPEG_MTK_BASE + 15)
#define V4L2_CID_MPEG_MTK_OPERATING_RATE (V4L2_CID_MPEG_MTK_BASE + 16)
#define V4L2_CID_MPEG_MTK_SEC_ENCODE (V4L2_CID_MPEG_MTK_BASE + 17)
#define V4L2_CID_MPEG_MTK_QUEUED_FRAMEBUF_COUNT (V4L2_CID_MPEG_MTK_BASE + 18)
#define V4L2_CID_MPEG_MTK_ENCODE_SCENARIO (V4L2_CID_MPEG_MTK_BASE + 20)
#define V4L2_CID_MPEG_MTK_ENCODE_NONREFP (V4L2_CID_MPEG_MTK_BASE + 21)
#define V4L2_CID_MPEG_MTK_ENCODE_DETECTED_FRAMERATE (V4L2_CID_MPEG_MTK_BASE + 22)
#define V4L2_CID_MPEG_MTK_ENCODE_RFS_ON (V4L2_CID_MPEG_MTK_BASE + 23)
#define V4L2_CID_MPEG_MTK_ENCODE_OPERATION_RATE (V4L2_CID_MPEG_MTK_BASE + 24)
#define V4L2_CID_MPEG_MTK_ENCODE_ROI_RC_QP (V4L2_CID_MPEG_MTK_BASE + 25)
#define V4L2_CID_MPEG_MTK_ENCODE_ROI_ON (V4L2_CID_MPEG_MTK_BASE + 26)
#define V4L2_CID_MPEG_MTK_ENCODE_GRID_SIZE (V4L2_CID_MPEG_MTK_BASE + 27)
#define V4L2_CID_MPEG_MTK_RESOLUTION_CHANGE (V4L2_CID_MPEG_MTK_BASE + 28)
#define V4L2_CID_MPEG_MTK_MAX_WIDTH (V4L2_CID_MPEG_MTK_BASE + 29)
#define V4L2_CID_MPEG_MTK_MAX_HEIGHT (V4L2_CID_MPEG_MTK_BASE + 30)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_I_FRAME_QP (V4L2_CID_MPEG_MTK_BASE + 31)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_P_FRAME_QP (V4L2_CID_MPEG_MTK_BASE + 32)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_B_FRAME_QP (V4L2_CID_MPEG_MTK_BASE + 33)
#define V4L2_CID_MPEG_VIDEO_ENABLE_TSVC (V4L2_CID_MPEG_MTK_BASE + 34)
#define V4L2_CID_MPEG_MTK_ENCODE_NONREFP_FREQ (V4L2_CID_MPEG_MTK_BASE + 35)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_MAX_QP (V4L2_CID_MPEG_MTK_BASE + 36)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_MIN_QP (V4L2_CID_MPEG_MTK_BASE + 37)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_I_P_QP_DELTA (V4L2_CID_MPEG_MTK_BASE + 38)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_QP_CONTROL_MODE (V4L2_CID_MPEG_MTK_BASE + 39)
#define V4L2_CID_MPEG_MTK_ENCODE_RC_FRAME_LEVEL_QP (V4L2_CID_MPEG_MTK_BASE + 40)
#define V4L2_CID_MPEG_MTK_ENCODE_MAX_REFP_NUM (V4L2_CID_MPEG_MTK_BASE + 41)
#define V4L2_CID_MPEG_MTK_ENCODE_REFP_DISTANCE (V4L2_CID_MPEG_MTK_BASE + 42)
#define V4L2_CID_MPEG_MTK_ENCODE_REFP_MAX_FRAME_NUM (V4L2_CID_MPEG_MTK_BASE + 43)
#define V4L2_CID_MPEG_MTK_ENCODE_REFP_FRAME_NUM (V4L2_CID_MPEG_MTK_BASE + 44)
#define V4L2_CID_MPEG_MTK_LOG (V4L2_CID_MPEG_MTK_BASE + 46)
#define V4L2_CID_MPEG_MTK_ENCODE_ENABLE_DUMMY_NAL (V4L2_CID_MPEG_MTK_BASE + 47)
#endif

static inline struct vb2_buffer *
mtk_v4l2_m2m_next_src_buf(struct v4l2_m2m_ctx *m2m_ctx)
{
	struct vb2_v4l2_buffer *buffer = v4l2_m2m_next_src_buf(m2m_ctx);

	return buffer ? &buffer->vb2_buf : NULL;
}

static inline struct vb2_buffer *
mtk_v4l2_m2m_next_dst_buf(struct v4l2_m2m_ctx *m2m_ctx)
{
	struct vb2_v4l2_buffer *buffer = v4l2_m2m_next_dst_buf(m2m_ctx);

	return buffer ? &buffer->vb2_buf : NULL;
}

static inline struct vb2_buffer *
mtk_v4l2_m2m_src_buf_remove(struct v4l2_m2m_ctx *m2m_ctx)
{
	struct vb2_v4l2_buffer *buffer = v4l2_m2m_src_buf_remove(m2m_ctx);

	return buffer ? &buffer->vb2_buf : NULL;
}

static inline struct vb2_buffer *
mtk_v4l2_m2m_dst_buf_remove(struct v4l2_m2m_ctx *m2m_ctx)
{
	struct vb2_v4l2_buffer *buffer = v4l2_m2m_dst_buf_remove(m2m_ctx);

	return buffer ? &buffer->vb2_buf : NULL;
}

static inline u64
mtk_v4l2_timeval_to_ns(const struct __kernel_v4l2_timeval *timeval)
{
	return (u64)timeval->tv_sec * NSEC_PER_SEC +
		(u64)timeval->tv_usec * NSEC_PER_USEC;
}

#define v4l2_m2m_next_src_buf mtk_v4l2_m2m_next_src_buf
#define v4l2_m2m_next_dst_buf mtk_v4l2_m2m_next_dst_buf
#define v4l2_m2m_src_buf_remove mtk_v4l2_m2m_src_buf_remove
#define v4l2_m2m_dst_buf_remove mtk_v4l2_m2m_dst_buf_remove

#endif
