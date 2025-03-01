/**************************************************************************
 *
 * Copyright 2010 Thomas Balling Sørensen & Orasanu Lucian.
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "pipe/p_screen.h"

#include "util/u_video.h"
#include "util/u_memory.h"

#include "vl/vl_winsys.h"
#include "vl/vl_codec.h"

#include "va_private.h"

#include "util/u_handle_table.h"

DEBUG_GET_ONCE_BOOL_OPTION(mpeg4, "VAAPI_MPEG4_ENABLED", false)

VAStatus
vlVaQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles)
{
   struct pipe_screen *pscreen;
   enum pipe_video_profile p;
   VAProfile vap;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   *num_profiles = 0;

   pscreen = VL_VA_PSCREEN(ctx);
   for (p = PIPE_VIDEO_PROFILE_MPEG2_SIMPLE; p <= PIPE_VIDEO_PROFILE_AV1_MAIN; ++p) {
      if (u_reduce_video_profile(p) == PIPE_VIDEO_FORMAT_MPEG4 && !debug_get_option_mpeg4())
         continue;

      if (vl_codec_supported(pscreen, p, false) ||
          vl_codec_supported(pscreen, p, true)) {
         vap = PipeToProfile(p);
         if (vap != VAProfileNone)
            profile_list[(*num_profiles)++] = vap;
      }
   }

   /* Support postprocessing through vl_compositor */
   profile_list[(*num_profiles)++] = VAProfileNone;

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                           VAEntrypoint *entrypoint_list, int *num_entrypoints)
{
   struct pipe_screen *pscreen;
   enum pipe_video_profile p;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   *num_entrypoints = 0;

   if (profile == VAProfileNone) {
      entrypoint_list[(*num_entrypoints)++] = VAEntrypointVideoProc;
      return VA_STATUS_SUCCESS;
   }

   p = ProfileToPipe(profile);
   if (p == PIPE_VIDEO_PROFILE_UNKNOWN ||
      (u_reduce_video_profile(p) == PIPE_VIDEO_FORMAT_MPEG4 &&
      !debug_get_option_mpeg4()))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

   pscreen = VL_VA_PSCREEN(ctx);
   if (vl_codec_supported(pscreen, p, false))
      entrypoint_list[(*num_entrypoints)++] = VAEntrypointVLD;

   if (vl_codec_supported(pscreen, p, true))
      entrypoint_list[(*num_entrypoints)++] = VAEntrypointEncSlice;

   if (*num_entrypoints == 0)
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

   assert(*num_entrypoints <= ctx->max_entrypoints);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaGetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                        VAConfigAttrib *attrib_list, int num_attribs)
{
   struct pipe_screen *pscreen;
   int i;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   pscreen = VL_VA_PSCREEN(ctx);

   for (i = 0; i < num_attribs; ++i) {
      unsigned int value;
      if ((entrypoint == VAEntrypointVLD) &&
          (vl_codec_supported(pscreen, ProfileToPipe(profile), false))) {
         switch (attrib_list[i].type) {
         case VAConfigAttribRTFormat:
            value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422;
            if (pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P010,
                                                   ProfileToPipe(profile),
                                                   PIPE_VIDEO_ENTRYPOINT_BITSTREAM) ||
                pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P016,
                                                   ProfileToPipe(profile),
                                                   PIPE_VIDEO_ENTRYPOINT_BITSTREAM))
               value |= VA_RT_FORMAT_YUV420_10BPP;
            break;
         default:
            value = VA_ATTRIB_NOT_SUPPORTED;
            break;
         }
      } else if ((entrypoint == VAEntrypointEncSlice) &&
                 (vl_codec_supported(pscreen, ProfileToPipe(profile), true))) {
         switch (attrib_list[i].type) {
         case VAConfigAttribRTFormat:
            value = VA_RT_FORMAT_YUV420;
            if (pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P010,
                                                   ProfileToPipe(profile),
                                                   PIPE_VIDEO_ENTRYPOINT_ENCODE) ||
                pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P016,
                                                   ProfileToPipe(profile),
                                                   PIPE_VIDEO_ENTRYPOINT_ENCODE))
               value |= VA_RT_FORMAT_YUV420_10BPP;
            break;
         case VAConfigAttribRateControl:
            value = VA_RC_CQP | VA_RC_CBR | VA_RC_VBR;
            break;
         case VAConfigAttribEncRateControlExt:
            value = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                             PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                             PIPE_VIDEO_CAP_MAX_TEMPORAL_LAYERS);
            if (value > 0) {
               value -= 1;
               value |= (1 << 8);   /* temporal_layer_bitrate_control_flag */
            }
            break;
         case VAConfigAttribEncPackedHeaders:
            value = VA_ENC_PACKED_HEADER_NONE;
            if (u_reduce_video_profile(ProfileToPipe(profile)) == PIPE_VIDEO_FORMAT_HEVC)
               value |= VA_ENC_PACKED_HEADER_SEQUENCE;
            break;
         case VAConfigAttribEncMaxSlices:
         {
            /**
             * \brief Maximum number of slices per frame. Read-only.
             *
             * This attribute determines the maximum number of slices the
             * driver can support to encode a single frame.
             */
            int maxSlicesPerEncodedPic = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                             PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                             PIPE_VIDEO_CAP_ENC_MAX_SLICES_PER_FRAME);
            if (maxSlicesPerEncodedPic <= 0)
               value = VA_ATTRIB_NOT_SUPPORTED;
            else
               value = maxSlicesPerEncodedPic;
         } break;
         case VAConfigAttribEncMaxRefFrames:
         {
            int maxL0L1ReferencesPerFrame = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                             PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                             PIPE_VIDEO_CAP_ENC_MAX_REFERENCES_PER_FRAME);
            if (maxL0L1ReferencesPerFrame <= 0)
               value = 1;
            else
               value = maxL0L1ReferencesPerFrame;
         } break;
         case VAConfigAttribEncSliceStructure:
         {
            /* The VA enum values match the pipe_video_cap_slice_structure definitions*/
            int supportedSliceStructuresFlagSet = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                             PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                             PIPE_VIDEO_CAP_ENC_SLICES_STRUCTURE);
            if (supportedSliceStructuresFlagSet <= 0)
               value = VA_ATTRIB_NOT_SUPPORTED;
            else
               value = supportedSliceStructuresFlagSet;
         } break;
         case VAConfigAttribEncQualityRange:
         {
            /*
             * this quality range provides different options within the range; and it isn't strictly
             * faster when higher value used.
             * 0, not used; 1, default value; others are using vlVaQualityBits for different modes.
             */
            int quality_range = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                 PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                 PIPE_VIDEO_CAP_ENC_QUALITY_LEVEL);
            value = quality_range ? quality_range : VA_ATTRIB_NOT_SUPPORTED;
         } break;
         case VAConfigAttribMaxFrameSize:
         {
            /* Max Frame Size can be used to control picture level frame size.
             * This frame size is in bits.
             */
            value = pscreen->get_video_param(pscreen, ProfileToPipe(profile),
                                             PIPE_VIDEO_ENTRYPOINT_ENCODE,
                                             PIPE_VIDEO_CAP_ENC_SUPPORTS_MAX_FRAME_SIZE);
            value = value ? value : VA_ATTRIB_NOT_SUPPORTED;
         } break;
         default:
            value = VA_ATTRIB_NOT_SUPPORTED;
            break;
         }
      } else if (entrypoint == VAEntrypointVideoProc) {
         switch (attrib_list[i].type) {
         case VAConfigAttribRTFormat:
            value = (VA_RT_FORMAT_YUV420 |
                     VA_RT_FORMAT_YUV420_10BPP |
                     VA_RT_FORMAT_RGB32);
            break;
         default:
            value = VA_ATTRIB_NOT_SUPPORTED;
            break;
         }
      } else {
         value = VA_ATTRIB_NOT_SUPPORTED;
      }
      attrib_list[i].value = value;
   }

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaCreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                 VAConfigAttrib *attrib_list, int num_attribs, VAConfigID *config_id)
{
   vlVaDriver *drv;
   vlVaConfig *config;
   struct pipe_screen *pscreen;
   enum pipe_video_profile p;
   unsigned int supported_rt_formats;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   config = CALLOC(1, sizeof(vlVaConfig));
   if (!config)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   if (profile == VAProfileNone) {
      if (entrypoint != VAEntrypointVideoProc) {
         FREE(config);
         return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
      }

      config->entrypoint = PIPE_VIDEO_ENTRYPOINT_PROCESSING;
      config->profile = PIPE_VIDEO_PROFILE_UNKNOWN;
      supported_rt_formats = VA_RT_FORMAT_YUV420 |
                             VA_RT_FORMAT_YUV420_10BPP |
                             VA_RT_FORMAT_RGB32;
      for (int i = 0; i < num_attribs; i++) {
         if (attrib_list[i].type == VAConfigAttribRTFormat) {
            if (attrib_list[i].value & supported_rt_formats) {
               config->rt_format = attrib_list[i].value;
            } else {
               FREE(config);
               return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
         } else {
            /*other attrib_types are not supported.*/
            FREE(config);
            return VA_STATUS_ERROR_INVALID_VALUE;
         }
      }

      /* Default value if not specified in the input attributes. */
      if (!config->rt_format)
         config->rt_format = supported_rt_formats;

      mtx_lock(&drv->mutex);
      *config_id = handle_table_add(drv->htab, config);
      mtx_unlock(&drv->mutex);
      return VA_STATUS_SUCCESS;
   }

   p = ProfileToPipe(profile);
   if (p == PIPE_VIDEO_PROFILE_UNKNOWN  ||
      (u_reduce_video_profile(p) == PIPE_VIDEO_FORMAT_MPEG4 &&
      !debug_get_option_mpeg4())) {
      FREE(config);
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
   }

   pscreen = VL_VA_PSCREEN(ctx);

   switch (entrypoint) {
   case VAEntrypointVLD:
      supported_rt_formats = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422;
      if (!vl_codec_supported(pscreen, p, false)) {
         FREE(config);
         return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
      }

      config->entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
      break;

   case VAEntrypointEncSlice:
      supported_rt_formats = VA_RT_FORMAT_YUV420;
      if (!vl_codec_supported(pscreen, p, true)) {
         FREE(config);
         return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
      }

      config->entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
      break;

   default:
      FREE(config);
      return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
   }

   config->profile = p;
   if (pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P010, p,
         config->entrypoint) ||
       pscreen->is_video_format_supported(pscreen, PIPE_FORMAT_P016, p,
         config->entrypoint))
      supported_rt_formats |= VA_RT_FORMAT_YUV420_10BPP;

   for (int i = 0; i <num_attribs ; i++) {
      if (attrib_list[i].type != VAConfigAttribRTFormat &&
         entrypoint == VAEntrypointVLD ) {
         FREE(config);
         return VA_STATUS_ERROR_INVALID_VALUE;
      }
      if (attrib_list[i].type == VAConfigAttribRateControl) {
         if (attrib_list[i].value == VA_RC_CBR)
            config->rc = PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT;
         else if (attrib_list[i].value == VA_RC_VBR)
            config->rc = PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE;
         else if (attrib_list[i].value == VA_RC_CQP)
            config->rc = PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE;
         else {
            FREE(config);
            return VA_STATUS_ERROR_INVALID_VALUE;
         }
      }
      if (attrib_list[i].type == VAConfigAttribRTFormat) {
         if (attrib_list[i].value & supported_rt_formats) {
            config->rt_format = attrib_list[i].value;
         } else {
            FREE(config);
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
         }
      }
      if (attrib_list[i].type == VAConfigAttribEncPackedHeaders) {
         if ((attrib_list[i].value > 1) ||
             (attrib_list[i].value &&
               u_reduce_video_profile(ProfileToPipe(profile)) !=
                  PIPE_VIDEO_FORMAT_HEVC) ||
             (config->entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE)) {
            FREE(config);
            return VA_STATUS_ERROR_INVALID_VALUE;
         }
      }
   }

   /* Default value if not specified in the input attributes. */
   if (!config->rt_format)
      config->rt_format = supported_rt_formats;

   mtx_lock(&drv->mutex);
   *config_id = handle_table_add(drv->htab, config);
   mtx_unlock(&drv->mutex);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaDestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
   vlVaDriver *drv;
   vlVaConfig *config;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   mtx_lock(&drv->mutex);
   config = handle_table_get(drv->htab, config_id);

   if (!config) {
      mtx_unlock(&drv->mutex);
      return VA_STATUS_ERROR_INVALID_CONFIG;
   }

   FREE(config);
   handle_table_remove(drv->htab, config_id);
   mtx_unlock(&drv->mutex);

   return VA_STATUS_SUCCESS;
}

VAStatus
vlVaQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile *profile,
                          VAEntrypoint *entrypoint, VAConfigAttrib *attrib_list, int *num_attribs)
{
   vlVaDriver *drv;
   vlVaConfig *config;

   if (!ctx)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   drv = VL_VA_DRIVER(ctx);

   if (!drv)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   mtx_lock(&drv->mutex);
   config = handle_table_get(drv->htab, config_id);
   mtx_unlock(&drv->mutex);

   if (!config)
      return VA_STATUS_ERROR_INVALID_CONFIG;

   *profile = PipeToProfile(config->profile);

   switch (config->entrypoint) {
   case PIPE_VIDEO_ENTRYPOINT_BITSTREAM:
      *entrypoint = VAEntrypointVLD;
      break;
   case PIPE_VIDEO_ENTRYPOINT_ENCODE:
      *entrypoint = VAEntrypointEncSlice;
      break;
   case PIPE_VIDEO_ENTRYPOINT_PROCESSING:
      *entrypoint = VAEntrypointVideoProc;
      break;
   default:
      return VA_STATUS_ERROR_INVALID_CONFIG;
   }

   *num_attribs = 1;
   attrib_list[0].type = VAConfigAttribRTFormat;
   attrib_list[0].value = config->rt_format;

   return VA_STATUS_SUCCESS;
}
