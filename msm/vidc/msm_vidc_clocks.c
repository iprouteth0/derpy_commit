// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
 */

#include "msm_vidc_common.h"
#include "vidc_hfi_api.h"
#include "msm_vidc_debug.h"
#include "msm_vidc_clocks.h"
#include "msm_vidc_buffer_calculations.h"
#include "msm_vidc_bus.h"

#define MSM_VIDC_MIN_UBWC_COMPLEXITY_FACTOR (1 << 16)
#define MSM_VIDC_MAX_UBWC_COMPLEXITY_FACTOR (4 << 16)

#define MSM_VIDC_MIN_UBWC_COMPRESSION_RATIO (1 << 16)
#define MSM_VIDC_MAX_UBWC_COMPRESSION_RATIO (5 << 16)

static int msm_vidc_decide_work_mode_ar50(struct msm_vidc_inst *inst);
static unsigned long msm_vidc_calc_freq_ar50(struct msm_vidc_inst *inst,
	u32 filled_len);
static unsigned long msm_vidc_calc_freq_iris1(struct msm_vidc_inst *inst,
	u32 filled_len);
static unsigned long msm_vidc_calc_freq_iris2(struct msm_vidc_inst *inst,
	u32 filled_len);

struct msm_vidc_core_ops core_ops_ar50 = {
	.calc_freq = msm_vidc_calc_freq_ar50,
	.decide_work_route = NULL,
	.decide_work_mode = msm_vidc_decide_work_mode_ar50,
	.decide_core_and_power_mode = NULL,
};

struct msm_vidc_core_ops core_ops_iris1 = {
	.calc_freq = msm_vidc_calc_freq_iris1,
	.decide_work_route = msm_vidc_decide_work_route_iris1,
	.decide_work_mode = msm_vidc_decide_work_mode_iris1,
	.decide_core_and_power_mode = msm_vidc_decide_core_and_power_mode_iris1,
};

struct msm_vidc_core_ops core_ops_iris2 = {
	.calc_freq = msm_vidc_calc_freq_iris2,
	.decide_work_route = msm_vidc_decide_work_route_iris2,
	.decide_work_mode = msm_vidc_decide_work_mode_iris2,
	.decide_core_and_power_mode = msm_vidc_decide_core_and_power_mode_iris2,
};

static inline void msm_dcvs_print_dcvs_stats(struct clock_data *dcvs)
{
	dprintk(VIDC_PERF,
		"DCVS: Load_Low %lld, Load Norm %lld, Load High %lld\n",
		dcvs->load_low,
		dcvs->load_norm,
		dcvs->load_high);

	dprintk(VIDC_PERF,
		"DCVS: min_threshold %d, max_threshold %d\n",
		dcvs->min_threshold, dcvs->max_threshold);
}

static inline unsigned long get_ubwc_compression_ratio(
	struct ubwc_cr_stats_info_type ubwc_stats_info)
{
	unsigned long sum = 0, weighted_sum = 0;
	unsigned long compression_ratio = 0;

	weighted_sum =
		32  * ubwc_stats_info.cr_stats_info0 +
		64  * ubwc_stats_info.cr_stats_info1 +
		96  * ubwc_stats_info.cr_stats_info2 +
		128 * ubwc_stats_info.cr_stats_info3 +
		160 * ubwc_stats_info.cr_stats_info4 +
		192 * ubwc_stats_info.cr_stats_info5 +
		256 * ubwc_stats_info.cr_stats_info6;

	sum =
		ubwc_stats_info.cr_stats_info0 +
		ubwc_stats_info.cr_stats_info1 +
		ubwc_stats_info.cr_stats_info2 +
		ubwc_stats_info.cr_stats_info3 +
		ubwc_stats_info.cr_stats_info4 +
		ubwc_stats_info.cr_stats_info5 +
		ubwc_stats_info.cr_stats_info6;

	compression_ratio = (weighted_sum && sum) ?
		((256 * sum) << 16) / weighted_sum : compression_ratio;

	return compression_ratio;
}

bool res_is_less_than(u32 width, u32 height,
			u32 ref_width, u32 ref_height)
{
	u32 num_mbs = NUM_MBS_PER_FRAME(height, width);
	u32 max_side = max(ref_width, ref_height);

	if (num_mbs < NUM_MBS_PER_FRAME(ref_height, ref_width) &&
		width < max_side &&
		height < max_side)
		return true;
	else
		return false;
}

bool res_is_greater_than(u32 width, u32 height,
				u32 ref_width, u32 ref_height)
{
	u32 num_mbs = NUM_MBS_PER_FRAME(height, width);
	u32 max_side = max(ref_width, ref_height);

	if (num_mbs > NUM_MBS_PER_FRAME(ref_height, ref_width) ||
		width > max_side ||
		height > max_side)
		return true;
	else
		return false;
}

bool res_is_greater_than_or_equal_to(u32 width, u32 height,
				u32 ref_width, u32 ref_height)
{
	u32 num_mbs = NUM_MBS_PER_FRAME(height, width);
	u32 max_side = max(ref_width, ref_height);

	if (num_mbs >= NUM_MBS_PER_FRAME(ref_height, ref_width) ||
		width >= max_side ||
		height >= max_side)
		return true;
	else
		return false;
}

bool res_is_less_than_or_equal_to(u32 width, u32 height,
				u32 ref_width, u32 ref_height)
{
	u32 num_mbs = NUM_MBS_PER_FRAME(height, width);
	u32 max_side = max(ref_width, ref_height);

	if (num_mbs <= NUM_MBS_PER_FRAME(ref_height, ref_width) ||
		width <= max_side ||
		height <= max_side)
		return true;
	else
		return false;
}

int msm_vidc_get_mbs_per_frame(struct msm_vidc_inst *inst)
{
	int height, width;
	struct v4l2_format *out_f;
	struct v4l2_format *inp_f;

	out_f = &inst->fmts[OUTPUT_PORT].v4l2_fmt;
	inp_f = &inst->fmts[INPUT_PORT].v4l2_fmt;
	height = max(out_f->fmt.pix_mp.height,
			inp_f->fmt.pix_mp.height);
	width = max(out_f->fmt.pix_mp.width,
			inp_f->fmt.pix_mp.width);

	return NUM_MBS_PER_FRAME(height, width);
}

int msm_vidc_get_fps(struct msm_vidc_inst *inst)
{
	int fps;

	if (inst->clk_data.operating_rate > inst->clk_data.frame_rate)
		fps = (inst->clk_data.operating_rate >> 16) ?
			(inst->clk_data.operating_rate >> 16) : 1;
	else
		fps = inst->clk_data.frame_rate >> 16;

	return fps;
}

void update_recon_stats(struct msm_vidc_inst *inst,
	struct recon_stats_type *recon_stats)
{
	struct v4l2_ctrl *ctrl;
	struct recon_buf *binfo;
	u32 CR = 0, CF = 0;
	u32 frame_size;

	/* do not consider recon stats in case of superframe */
	ctrl = get_ctrl(inst, V4L2_CID_MPEG_VIDC_SUPERFRAME);
	if (ctrl->val)
		return;

	CR = get_ubwc_compression_ratio(recon_stats->ubwc_stats_info);

	frame_size = (msm_vidc_get_mbs_per_frame(inst) / (32 * 8) * 3) / 2;

	if (frame_size)
		CF = recon_stats->complexity_number / frame_size;
	else
		CF = MSM_VIDC_MAX_UBWC_COMPLEXITY_FACTOR;
	mutex_lock(&inst->refbufs.lock);
	list_for_each_entry(binfo, &inst->refbufs.list, list) {
		if (binfo->buffer_index ==
				recon_stats->buffer_index) {
			binfo->CR = CR;
			binfo->CF = CF;
			break;
		}
	}
	mutex_unlock(&inst->refbufs.lock);
}

static int fill_dynamic_stats(struct msm_vidc_inst *inst,
	struct vidc_bus_vote_data *vote_data)
{
	struct recon_buf *binfo, *nextb;
	struct vidc_input_cr_data *temp, *next;
	u32 max_cr = MSM_VIDC_MIN_UBWC_COMPRESSION_RATIO;
	u32 max_cf = MSM_VIDC_MIN_UBWC_COMPLEXITY_FACTOR;
	u32 max_input_cr = MSM_VIDC_MIN_UBWC_COMPRESSION_RATIO;
	u32 min_cf = MSM_VIDC_MAX_UBWC_COMPLEXITY_FACTOR;
	u32 min_input_cr = MSM_VIDC_MAX_UBWC_COMPRESSION_RATIO;
	u32 min_cr = MSM_VIDC_MAX_UBWC_COMPRESSION_RATIO;

	mutex_lock(&inst->refbufs.lock);
	list_for_each_entry_safe(binfo, nextb, &inst->refbufs.list, list) {
		if (binfo->CR) {
			min_cr = min(min_cr, binfo->CR);
			max_cr = max(max_cr, binfo->CR);
		}
		if (binfo->CF) {
			min_cf = min(min_cf, binfo->CF);
			max_cf = max(max_cf, binfo->CF);
		}
	}
	mutex_unlock(&inst->refbufs.lock);

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		min_input_cr = min(min_input_cr, temp->input_cr);
		max_input_cr = max(max_input_cr, temp->input_cr);
	}
	mutex_unlock(&inst->input_crs.lock);

	/* Sanitize CF values from HW . */
	max_cf = min_t(u32, max_cf, MSM_VIDC_MAX_UBWC_COMPLEXITY_FACTOR);
	min_cf = max_t(u32, min_cf, MSM_VIDC_MIN_UBWC_COMPLEXITY_FACTOR);
	max_cr = min_t(u32, max_cr, MSM_VIDC_MAX_UBWC_COMPRESSION_RATIO);
	min_cr = max_t(u32, min_cr, MSM_VIDC_MIN_UBWC_COMPRESSION_RATIO);
	max_input_cr = min_t(u32,
		max_input_cr, MSM_VIDC_MAX_UBWC_COMPRESSION_RATIO);
	min_input_cr = max_t(u32,
		min_input_cr, MSM_VIDC_MIN_UBWC_COMPRESSION_RATIO);

	vote_data->compression_ratio = min_cr;
	vote_data->complexity_factor = max_cf;
	vote_data->input_cr = min_input_cr;

	dprintk(VIDC_PERF,
		"Input CR = %d Recon CR = %d Complexity Factor = %d\n",
			vote_data->input_cr, vote_data->compression_ratio,
			vote_data->complexity_factor);

	return 0;
}

int msm_comm_vote_bus(struct msm_vidc_core *core)
{
	int rc = 0, vote_data_count = 0, i = 0;
	struct hfi_device *hdev;
	struct msm_vidc_inst *inst = NULL;
	struct vidc_bus_vote_data *vote_data = NULL;
	bool is_turbo = false;
	struct v4l2_format *out_f;
	struct v4l2_format *inp_f;

	if (!core || !core->device) {
		dprintk(VIDC_ERR, "%s Invalid args: %pK\n", __func__, core);
		return -EINVAL;
	}
	hdev = core->device;

	vote_data = kzalloc(sizeof(struct vidc_bus_vote_data) *
			MAX_SUPPORTED_INSTANCES, GFP_ATOMIC);
	if (!vote_data) {
		dprintk(VIDC_LOW,
			"vote_data allocation with GFP_ATOMIC failed\n");
		vote_data = kzalloc(sizeof(struct vidc_bus_vote_data) *
			MAX_SUPPORTED_INSTANCES, GFP_KERNEL);
		if (!vote_data) {
			dprintk(VIDC_ERR,
				"vote_data allocation failed\n");
			return -EINVAL;
		}
	}

	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		int codec = 0;
		struct msm_vidc_buffer *temp, *next;
		u32 filled_len = 0;
		u32 device_addr = 0;

		mutex_lock(&inst->registeredbufs.lock);
		list_for_each_entry_safe(temp, next,
				&inst->registeredbufs.list, list) {
			if (temp->vvb.vb2_buf.type == INPUT_MPLANE) {
				filled_len = max(filled_len,
					temp->vvb.vb2_buf.planes[0].bytesused);
				device_addr = temp->smem[0].device_addr;
			}
			if (inst->session_type == MSM_VIDC_ENCODER &&
				(temp->vvb.flags &
				V4L2_BUF_FLAG_PERF_MODE)) {
				is_turbo = true;
			}
		}
		mutex_unlock(&inst->registeredbufs.lock);

		if ((!filled_len || !device_addr) &&
			(inst->session_type != MSM_VIDC_CVP)) {
			dprintk(VIDC_LOW, "%s: no input for session %x\n",
				__func__, hash32_ptr(inst->session));
			continue;
		}

		++vote_data_count;

		out_f = &inst->fmts[OUTPUT_PORT].v4l2_fmt;
		inp_f = &inst->fmts[INPUT_PORT].v4l2_fmt;
		switch (inst->session_type) {
		case MSM_VIDC_DECODER:
			codec = inp_f->fmt.pix_mp.pixelformat;
			break;
		case MSM_VIDC_ENCODER:
			codec = out_f->fmt.pix_mp.pixelformat;
			break;
		case MSM_VIDC_CVP:
			codec = V4L2_PIX_FMT_CVP;
			break;
		default:
			dprintk(VIDC_ERR, "%s: invalid session_type %#x\n",
				__func__, inst->session_type);
			break;
		}

		memset(&(vote_data[i]), 0x0, sizeof(struct vidc_bus_vote_data));

		vote_data[i].domain = get_hal_domain(inst->session_type);
		vote_data[i].codec = get_hal_codec(codec);
		vote_data[i].input_width = inp_f->fmt.pix_mp.width;
		vote_data[i].input_height = inp_f->fmt.pix_mp.height;
		vote_data[i].output_width = out_f->fmt.pix_mp.width;
		vote_data[i].output_height = out_f->fmt.pix_mp.height;
		vote_data[i].lcu_size = (codec == V4L2_PIX_FMT_HEVC ||
				codec == V4L2_PIX_FMT_VP9) ? 32 : 16;

		vote_data[i].fps = msm_vidc_get_fps(inst);
		if (inst->session_type == MSM_VIDC_ENCODER) {
			vote_data[i].bitrate = inst->clk_data.bitrate;
			vote_data[i].rotation =
				msm_comm_g_ctrl_for_id(inst, V4L2_CID_ROTATE);
			vote_data[i].b_frames_enabled =
				msm_comm_g_ctrl_for_id(inst,
					V4L2_CID_MPEG_VIDEO_B_FRAMES) != 0;
			/* scale bitrate if operating rate is larger than fps */
			if (vote_data[i].fps > (inst->clk_data.frame_rate >> 16)
				&& (inst->clk_data.frame_rate >> 16)) {
				vote_data[i].bitrate = vote_data[i].bitrate /
				(inst->clk_data.frame_rate >> 16) *
				vote_data[i].fps;
			}
		} else if (inst->session_type == MSM_VIDC_DECODER) {
			vote_data[i].bitrate =
				filled_len * vote_data[i].fps * 8;
		}

		vote_data[i].power_mode = 0;
		if (inst->clk_data.buffer_counter < DCVS_FTB_WINDOW &&
			inst->session_type != MSM_VIDC_CVP)
			vote_data[i].power_mode = VIDC_POWER_TURBO;
		if (msm_vidc_clock_voting || is_turbo)
			vote_data[i].power_mode = VIDC_POWER_TURBO;

		if (msm_comm_get_stream_output_mode(inst) ==
				HAL_VIDEO_DECODER_PRIMARY) {
			vote_data[i].color_formats[0] =
				msm_comm_get_hal_uncompressed(
				inst->clk_data.opb_fourcc);
			vote_data[i].num_formats = 1;
		} else {
			vote_data[i].color_formats[0] =
				msm_comm_get_hal_uncompressed(
				inst->clk_data.dpb_fourcc);
			vote_data[i].color_formats[1] =
				msm_comm_get_hal_uncompressed(
				inst->clk_data.opb_fourcc);
			vote_data[i].num_formats = 2;
		}
		vote_data[i].work_mode = inst->clk_data.work_mode;
		fill_dynamic_stats(inst, &vote_data[i]);

		if (core->resources.sys_cache_res_set)
			vote_data[i].use_sys_cache = true;

		if (inst->session_type == MSM_VIDC_CVP) {
			vote_data[i].domain =
				get_hal_domain(inst->session_type);
			vote_data[i].ddr_bw = inst->clk_data.ddr_bw;
			vote_data[i].sys_cache_bw =
				inst->clk_data.sys_cache_bw;
		}

		i++;
	}
	mutex_unlock(&core->lock);
	if (vote_data_count)
		rc = call_hfi_op(hdev, vote_bus, hdev->hfi_device_data,
			vote_data, vote_data_count);

	kfree(vote_data);
	return rc;
}

static int msm_dcvs_scale_clocks(struct msm_vidc_inst *inst,
		unsigned long freq)
{
	int rc = 0;
	int bufs_with_fw = 0;
	int bufs_with_client = 0;
	struct msm_vidc_format *fmt;
	struct clock_data *dcvs;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR, "%s Invalid params\n", __func__);
		return -EINVAL;
	}

	/* assume no increment or decrement is required initially */
	inst->clk_data.dcvs_flags = 0;

	if (!inst->clk_data.dcvs_mode || inst->batch.enable) {
		dprintk(VIDC_LOW, "Skip DCVS (dcvs %d, batching %d)\n",
			inst->clk_data.dcvs_mode, inst->batch.enable);
		/* update load (freq) with normal value */
		inst->clk_data.load = inst->clk_data.load_norm;
		return 0;
	}

	dcvs = &inst->clk_data;

	if (is_decode_session(inst)) {
		bufs_with_fw = msm_comm_num_queued_bufs(inst, OUTPUT_MPLANE);
		fmt = &inst->fmts[OUTPUT_PORT];
	} else {
		bufs_with_fw = msm_comm_num_queued_bufs(inst, INPUT_MPLANE);
		fmt = &inst->fmts[INPUT_PORT];
	}
	/* +1 as one buffer is going to be queued after the function */
	bufs_with_fw += 1;
	bufs_with_client = fmt->count_actual - bufs_with_fw;

	/*
	 * PMS decides clock level based on below algo

	 * Limits :
	 * max_threshold : Client extra allocated buffers. Client
	 * reserves these buffers for it's smooth flow.
	 * min_output_buf : HW requested buffers for it's smooth
	 * flow of buffers.
	 * min_threshold : Driver requested extra buffers for PMS.

	 * 1) When buffers outside FW are reaching client's extra buffers,
	 *    FW is slow and will impact pipeline, Increase clock.
	 * 2) When pending buffers with FW are same as FW requested,
	 *    pipeline has cushion to absorb FW slowness, Decrease clocks.
	 * 3) When none of 1) or 2) FW is just fast enough to maintain
	 *    pipeline, request Right Clocks.
	 */

	if (bufs_with_client <= dcvs->max_threshold) {
		dcvs->load = dcvs->load_high;
		dcvs->dcvs_flags |= MSM_VIDC_DCVS_INCR;
	} else if (bufs_with_fw < (int) fmt->count_min) {
		dcvs->load = dcvs->load_low;
		dcvs->dcvs_flags |= MSM_VIDC_DCVS_DECR;
	} else {
		dcvs->load = dcvs->load_norm;
		dcvs->dcvs_flags = 0;
	}

	dprintk(VIDC_PERF,
		"DCVS: %x : total bufs %d outside fw %d max threshold %d with fw %d min bufs %d flags %#x\n",
		hash32_ptr(inst->session), fmt->count_actual,
		bufs_with_client, dcvs->max_threshold, bufs_with_fw,
		fmt->count_min, dcvs->dcvs_flags);
	return rc;
}

static void msm_vidc_update_freq_entry(struct msm_vidc_inst *inst,
	unsigned long freq, u32 device_addr, bool is_turbo)
{
	struct vidc_freq_data *temp, *next;
	bool found = false;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		if (temp->device_addr == device_addr) {
			temp->freq = freq;
			found = true;
			break;
		}
	}

	if (!found) {
		temp = kzalloc(sizeof(*temp), GFP_KERNEL);
		if (!temp) {
			dprintk(VIDC_ERR, "%s: malloc failure.\n", __func__);
			goto exit;
		}
		temp->freq = freq;
		temp->device_addr = device_addr;
		list_add_tail(&temp->list, &inst->freqs.list);
	}
	temp->turbo = !!is_turbo;
exit:
	mutex_unlock(&inst->freqs.lock);
}

void msm_vidc_clear_freq_entry(struct msm_vidc_inst *inst,
	u32 device_addr)
{
	struct vidc_freq_data *temp, *next;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		if (temp->device_addr == device_addr)
			temp->freq = 0;
	}
	mutex_unlock(&inst->freqs.lock);

	inst->clk_data.buffer_counter++;
}

static unsigned long msm_vidc_max_freq(struct msm_vidc_core *core)
{
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	unsigned long freq = 0;

	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	freq = allowed_clks_tbl[0].clock_rate;
	dprintk(VIDC_PERF, "Max rate = %lu\n", freq);
	return freq;
}

void msm_comm_free_freq_table(struct msm_vidc_inst *inst)
{
	struct vidc_freq_data *temp, *next;

	mutex_lock(&inst->freqs.lock);
	list_for_each_entry_safe(temp, next, &inst->freqs.list, list) {
		list_del(&temp->list);
		kfree(temp);
	}
	INIT_LIST_HEAD(&inst->freqs.list);
	mutex_unlock(&inst->freqs.lock);
}

void msm_comm_free_input_cr_table(struct msm_vidc_inst *inst)
{
	struct vidc_input_cr_data *temp, *next;

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		list_del(&temp->list);
		kfree(temp);
	}
	INIT_LIST_HEAD(&inst->input_crs.list);
	mutex_unlock(&inst->input_crs.lock);
}

void msm_comm_update_input_cr(struct msm_vidc_inst *inst,
	u32 index, u32 cr)
{
	struct vidc_input_cr_data *temp, *next;
	bool found = false;

	mutex_lock(&inst->input_crs.lock);
	list_for_each_entry_safe(temp, next, &inst->input_crs.list, list) {
		if (temp->index == index) {
			temp->input_cr = cr;
			found = true;
			break;
		}
	}

	if (!found) {
		temp = kzalloc(sizeof(*temp), GFP_KERNEL);
		if (!temp)  {
			dprintk(VIDC_ERR, "%s: malloc failure.\n", __func__);
			goto exit;
		}
		temp->index = index;
		temp->input_cr = cr;
		list_add_tail(&temp->list, &inst->input_crs.list);
	}
exit:
	mutex_unlock(&inst->input_crs.lock);
}

static unsigned long msm_vidc_calc_freq_ar50(struct msm_vidc_inst *inst,
	u32 filled_len)
{
	u64 freq = 0, vpp_cycles = 0, vsp_cycles = 0;
	u64 fw_cycles = 0, fw_vpp_cycles = 0;
	u32 vpp_cycles_per_mb;
	u32 mbs_per_second;
	struct msm_vidc_core *core = NULL;
	int i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 rate = 0, fps;
	struct clock_data *dcvs = NULL;

	core = inst->core;
	dcvs = &inst->clk_data;

	mbs_per_second = msm_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	fps = msm_vidc_get_fps(inst);

	/*
	 * Calculate vpp, vsp cycles separately for encoder and decoder.
	 * Even though, most part is common now, in future it may change
	 * between them.
	 */

	fw_cycles = fps * inst->core->resources.fw_cycles;
	fw_vpp_cycles = fps * inst->core->resources.fw_vpp_cycles;

	if (inst->session_type == MSM_VIDC_ENCODER) {
		vpp_cycles_per_mb = inst->flags & VIDC_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;

		vpp_cycles = mbs_per_second * vpp_cycles_per_mb;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* 10 / 7 is overhead factor */
		vsp_cycles += (inst->clk_data.bitrate * 10) / 7;
	} else if (inst->session_type == MSM_VIDC_DECODER) {
		vpp_cycles = mbs_per_second * inst->clk_data.entry->vpp_cycles;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;
		/* 10 / 7 is overhead factor */
		vsp_cycles += ((fps * filled_len * 8) * 10) / 7;

	} else {
		dprintk(VIDC_ERR, "Unknown session type = %s\n", __func__);
		return msm_vidc_max_freq(inst->core);
	}

	freq = max(vpp_cycles, vsp_cycles);
	freq = max(freq, fw_cycles);

	dprintk(VIDC_LOW, "Update DCVS Load\n");
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq)
			break;
	}

	dcvs->load_norm = rate;
	dcvs->load_low = i < (int) (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	msm_dcvs_print_dcvs_stats(dcvs);

	dprintk(VIDC_PERF, "%s Inst %pK : Filled Len = %d Freq = %llu\n",
		__func__, inst, filled_len, freq);

	return (unsigned long) freq;
}

static unsigned long msm_vidc_calc_freq_iris1(struct msm_vidc_inst *inst,
	u32 filled_len)
{
	u64 vsp_cycles = 0, vpp_cycles = 0, fw_cycles = 0, freq = 0;
	u64 fw_vpp_cycles = 0;
	u32 vpp_cycles_per_mb;
	u32 mbs_per_second;
	struct msm_vidc_core *core = NULL;
	int i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 rate = 0, fps;
	struct clock_data *dcvs = NULL;
	u32 operating_rate, vsp_factor_num = 10, vsp_factor_den = 5;

	core = inst->core;
	dcvs = &inst->clk_data;

	mbs_per_second = msm_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	fps = msm_vidc_get_fps(inst);

	/*
	 * Calculate vpp, vsp, fw cycles separately for encoder and decoder.
	 * Even though, most part is common now, in future it may change
	 * between them.
	 */

	fw_cycles = fps * inst->core->resources.fw_cycles;
	fw_vpp_cycles = fps * inst->core->resources.fw_vpp_cycles;

	if (inst->session_type == MSM_VIDC_ENCODER) {
		vpp_cycles_per_mb = inst->flags & VIDC_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;

		vpp_cycles = mbs_per_second * vpp_cycles_per_mb /
				inst->clk_data.work_route;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* bitrate is based on fps, scale it using operating rate */
		operating_rate = inst->clk_data.operating_rate >> 16;
		if (operating_rate > (inst->clk_data.frame_rate >> 16) &&
			(inst->clk_data.frame_rate >> 16)) {
			vsp_factor_num *= operating_rate;
			vsp_factor_den *= inst->clk_data.frame_rate >> 16;
		}
		vsp_cycles += ((u64)inst->clk_data.bitrate * vsp_factor_num) /
				vsp_factor_den;

	} else if (inst->session_type == MSM_VIDC_DECODER) {
		vpp_cycles = mbs_per_second * inst->clk_data.entry->vpp_cycles /
				inst->clk_data.work_route;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* vsp perf is about 0.5 bits/cycle */
		vsp_cycles += ((fps * filled_len * 8) * 10) / 5;

	} else {
		dprintk(VIDC_ERR, "Unknown session type = %s\n", __func__);
		return msm_vidc_max_freq(inst->core);
	}

	freq = max(vpp_cycles, vsp_cycles);
	freq = max(freq, fw_cycles);

	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq)
			break;
	}

	dcvs->load_norm = rate;
	dcvs->load_low = i < (int) (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	dprintk(VIDC_PERF,
		"%s: inst %pK: %x : filled len %d required freq %llu load_norm %llu\n",
		__func__, inst, hash32_ptr(inst->session),
		filled_len, freq, dcvs->load_norm);

	return (unsigned long) freq;
}

static unsigned long msm_vidc_calc_freq_iris2(struct msm_vidc_inst *inst,
	u32 filled_len)
{
	u64 vsp_cycles = 0, vpp_cycles = 0, fw_cycles = 0, freq = 0;
	u64 fw_vpp_cycles = 0;
	u32 vpp_cycles_per_mb;
	u32 mbs_per_second;
	struct msm_vidc_core *core = NULL;
	int i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 rate = 0, fps;
	struct clock_data *dcvs = NULL;
	u32 operating_rate, vsp_factor_num = 10, vsp_factor_den = 5;

	core = inst->core;
	dcvs = &inst->clk_data;

	mbs_per_second = msm_comm_get_inst_load_per_core(inst,
		LOAD_CALC_NO_QUIRKS);

	fps = msm_vidc_get_fps(inst);

	/*
	 * Calculate vpp, vsp, fw cycles separately for encoder and decoder.
	 * Even though, most part is common now, in future it may change
	 * between them.
	 */

	fw_cycles = fps * inst->core->resources.fw_cycles;
	fw_vpp_cycles = fps * inst->core->resources.fw_vpp_cycles;

	if (inst->session_type == MSM_VIDC_ENCODER) {
		vpp_cycles_per_mb = inst->flags & VIDC_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;

		vpp_cycles = mbs_per_second * vpp_cycles_per_mb /
				inst->clk_data.work_route;
		/* 1.25 factor for IbP GOP structure */
		if (msm_comm_g_ctrl_for_id(inst, V4L2_CID_MPEG_VIDEO_B_FRAMES))
			vpp_cycles += vpp_cycles / 4;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);
		/* 1.01 is multi-pipe overhead */
		if (inst->clk_data.work_route > 1)
			vpp_cycles += vpp_cycles / 100;

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* bitrate is based on fps, scale it using operating rate */
		operating_rate = inst->clk_data.operating_rate >> 16;
		if (operating_rate > (inst->clk_data.frame_rate >> 16) &&
			(inst->clk_data.frame_rate >> 16)) {
			vsp_factor_num *= operating_rate;
			vsp_factor_den *= inst->clk_data.frame_rate >> 16;
		}
		vsp_cycles += ((u64)inst->clk_data.bitrate * vsp_factor_num) /
				vsp_factor_den;
	} else if (inst->session_type == MSM_VIDC_DECODER) {
		vpp_cycles = mbs_per_second * inst->clk_data.entry->vpp_cycles /
				inst->clk_data.work_route;
		/* 21 / 20 is minimum overhead factor */
		vpp_cycles += max(vpp_cycles / 20, fw_vpp_cycles);
		/* 1.059 is multi-pipe overhead */
		if (inst->clk_data.work_route > 1)
			vpp_cycles += vpp_cycles * 59 / 1000;

		vsp_cycles = mbs_per_second * inst->clk_data.entry->vsp_cycles;

		/* vsp perf is about 0.5 bits/cycle */
		vsp_cycles += ((fps * filled_len * 8) * 10) / 5;
	} else {
		dprintk(VIDC_ERR, "Unknown session type = %s\n", __func__);
		return msm_vidc_max_freq(inst->core);
	}

	freq = max(vpp_cycles, vsp_cycles);
	freq = max(freq, fw_cycles);

	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq)
			break;
	}

	dcvs->load_norm = rate;
	dcvs->load_low = i < (int) (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	dprintk(VIDC_PERF,
		"%s: inst %pK: %x : filled len %d required freq %llu load_norm %llu\n",
		__func__, inst, hash32_ptr(inst->session),
		filled_len, freq, dcvs->load_norm);

	return (unsigned long) freq;
}

int msm_vidc_set_clocks(struct msm_vidc_core *core)
{
	struct hfi_device *hdev;
	unsigned long freq_core_1 = 0, freq_core_2 = 0, rate = 0;
	unsigned long freq_core_max = 0;
	struct msm_vidc_inst *inst = NULL;
	struct msm_vidc_buffer *temp, *next;
	u32 device_addr, filled_len;
	int rc = 0, i = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	bool increment, decrement;

	hdev = core->device;
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	if (!allowed_clks_tbl) {
		dprintk(VIDC_ERR,
			"%s Invalid parameters\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&core->lock);
	increment = false;
	decrement = true;
	list_for_each_entry(inst, &core->instances, list) {
		device_addr = 0;
		filled_len = 0;
		mutex_lock(&inst->registeredbufs.lock);
		list_for_each_entry_safe(temp, next,
				&inst->registeredbufs.list, list) {
			if (temp->vvb.vb2_buf.type == INPUT_MPLANE) {
				filled_len = max(filled_len,
					temp->vvb.vb2_buf.planes[0].bytesused);
				device_addr = temp->smem[0].device_addr;
			}
		}
		mutex_unlock(&inst->registeredbufs.lock);

		if (!filled_len || !device_addr) {
			dprintk(VIDC_LOW, "%s no input for session %x\n",
				__func__, hash32_ptr(inst->session));
			continue;
		}

		if (inst->clk_data.core_id == VIDC_CORE_ID_1)
			freq_core_1 += inst->clk_data.min_freq;
		else if (inst->clk_data.core_id == VIDC_CORE_ID_2)
			freq_core_2 += inst->clk_data.min_freq;
		else if (inst->clk_data.core_id == VIDC_CORE_ID_3) {
			freq_core_1 += inst->clk_data.min_freq;
			freq_core_2 += inst->clk_data.min_freq;
		}

		freq_core_max = max_t(unsigned long, freq_core_1, freq_core_2);

		if (msm_vidc_clock_voting) {
			dprintk(VIDC_PERF,
				"msm_vidc_clock_voting %d\n",
				 msm_vidc_clock_voting);
			freq_core_max = msm_vidc_clock_voting;
			decrement = false;
			break;
		}

		/* increment even if one session requested for it */
		if (inst->clk_data.dcvs_flags & MSM_VIDC_DCVS_INCR)
			increment = true;
		/* decrement only if all sessions requested for it */
		if (!(inst->clk_data.dcvs_flags & MSM_VIDC_DCVS_DECR))
			decrement = false;
	}

	/*
	 * keep checking from lowest to highest rate until
	 * table rate >= requested rate
	 */
	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= freq_core_max)
			break;
	}
	if (increment) {
		if (i > 0)
			rate = allowed_clks_tbl[i-1].clock_rate;
	} else if (decrement) {
		if (i < (int) (core->resources.allowed_clks_tbl_size - 1))
			rate = allowed_clks_tbl[i+1].clock_rate;
	}

	core->min_freq = freq_core_max;
	core->curr_freq = rate;
	mutex_unlock(&core->lock);

	dprintk(VIDC_PERF,
		"%s: clock rate %lu requested %lu increment %d decrement %d\n",
		__func__, core->curr_freq, core->min_freq,
		increment, decrement);
	rc = call_hfi_op(hdev, scale_clocks,
			hdev->hfi_device_data, core->curr_freq);

	return rc;
}

int msm_comm_scale_clocks(struct msm_vidc_inst *inst)
{
	struct msm_vidc_buffer *temp, *next;
	unsigned long freq = 0;
	u32 filled_len = 0;
	u32 device_addr = 0;
	bool is_turbo = false;

	if (!inst || !inst->core) {
		dprintk(VIDC_ERR, "%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	mutex_lock(&inst->registeredbufs.lock);
	list_for_each_entry_safe(temp, next, &inst->registeredbufs.list, list) {
		if (temp->vvb.vb2_buf.type == INPUT_MPLANE) {
			filled_len = max(filled_len,
				temp->vvb.vb2_buf.planes[0].bytesused);
			if (inst->session_type == MSM_VIDC_ENCODER &&
				(temp->vvb.flags &
				 V4L2_BUF_FLAG_PERF_MODE)) {
				is_turbo = true;
			}
			device_addr = temp->smem[0].device_addr;
		}
	}
	mutex_unlock(&inst->registeredbufs.lock);

	if (!filled_len || !device_addr) {
		dprintk(VIDC_LOW, "%s no input for session %x\n",
			__func__, hash32_ptr(inst->session));
		return 0;
	}

	freq = call_core_op(inst->core, calc_freq, inst, filled_len);
	inst->clk_data.min_freq = freq;
	/* update dcvs flags */
	msm_dcvs_scale_clocks(inst, freq);

	if (inst->clk_data.buffer_counter < DCVS_FTB_WINDOW || is_turbo ||
		msm_vidc_clock_voting) {
		inst->clk_data.min_freq = msm_vidc_max_freq(inst->core);
		inst->clk_data.dcvs_flags = 0;
	}

	msm_vidc_update_freq_entry(inst, freq, device_addr, is_turbo);

	msm_vidc_set_clocks(inst->core);

	return 0;
}

int msm_comm_scale_clocks_and_bus(struct msm_vidc_inst *inst)
{
	struct msm_vidc_core *core;
	struct hfi_device *hdev;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR, "%s Invalid params\n", __func__);
		return -EINVAL;
	}
	core = inst->core;
	hdev = core->device;

	if (msm_comm_scale_clocks(inst)) {
		dprintk(VIDC_ERR,
			"Failed to scale clocks. Performance might be impacted\n");
	}
	if (msm_comm_vote_bus(core)) {
		dprintk(VIDC_ERR,
			"Failed to scale DDR bus. Performance might be impacted\n");
	}
	return 0;
}

int msm_dcvs_try_enable(struct msm_vidc_inst *inst)
{
	if (!inst || !inst->core) {
		dprintk(VIDC_ERR, "%s: Invalid args: %p\n", __func__, inst);
		return -EINVAL;
	}

	if (msm_vidc_clock_voting ||
			!inst->core->resources.dcvs ||
			inst->flags & VIDC_THUMBNAIL ||
			inst->clk_data.low_latency_mode ||
			inst->batch.enable ||
			inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ ||
			inst->grid_enable) {
		dprintk(VIDC_HIGH, "DCVS disabled: %pK\n", inst);
		inst->clk_data.dcvs_mode = false;
		return false;
	}
	inst->clk_data.dcvs_mode = true;
	dprintk(VIDC_HIGH, "DCVS enabled: %pK\n", inst);

	return true;
}

int msm_comm_init_clocks_and_bus_data(struct msm_vidc_inst *inst)
{
	int rc = 0, j = 0;
	int fourcc, count;

	if (!inst || !inst->core) {
		dprintk(VIDC_ERR, "%s Invalid args: Inst = %pK\n",
				__func__, inst);
		return -EINVAL;
	}

	if (inst->session_type == MSM_VIDC_CVP) {
		dprintk(VIDC_LOW, "%s: cvp session\n", __func__);
		return 0;
	}

	count = inst->core->resources.codec_data_count;
	fourcc = get_v4l2_codec(inst);

	for (j = 0; j < count; j++) {
		if (inst->core->resources.codec_data[j].session_type ==
				inst->session_type &&
				inst->core->resources.codec_data[j].fourcc ==
				fourcc) {
			inst->clk_data.entry =
				&inst->core->resources.codec_data[j];
			break;
		}
	}

	if (!inst->clk_data.entry) {
		dprintk(VIDC_ERR, "%s No match found\n", __func__);
		rc = -EINVAL;
	}

	return rc;
}

void msm_clock_data_reset(struct msm_vidc_inst *inst)
{
	struct msm_vidc_core *core;
	int i = 0, rc = 0;
	struct allowed_clock_rates_table *allowed_clks_tbl = NULL;
	u64 total_freq = 0, rate = 0, load;
	int cycles;
	struct clock_data *dcvs;
	struct msm_vidc_format *fmt;

	dprintk(VIDC_HIGH, "Init DCVS Load\n");

	if (!inst || !inst->core || !inst->clk_data.entry) {
		dprintk(VIDC_ERR, "%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return;
	}

	core = inst->core;
	dcvs = &inst->clk_data;
	load = msm_comm_get_inst_load_per_core(inst, LOAD_CALC_NO_QUIRKS);
	cycles = inst->clk_data.entry->vpp_cycles;
	allowed_clks_tbl = core->resources.allowed_clks_tbl;
	if (inst->session_type == MSM_VIDC_ENCODER) {
		cycles = inst->flags & VIDC_LOW_POWER ?
			inst->clk_data.entry->low_power_cycles :
			cycles;

		dcvs->buffer_type = HAL_BUFFER_INPUT;
		dcvs->min_threshold =
			msm_vidc_get_extra_buff_count(inst, HAL_BUFFER_INPUT);
		fmt = &inst->fmts[INPUT_PORT];
		dcvs->max_threshold =
			fmt->count_actual - fmt->count_min_host + 2;
	} else if (inst->session_type == MSM_VIDC_DECODER) {
		dcvs->buffer_type = HAL_BUFFER_OUTPUT;
		fmt = &inst->fmts[OUTPUT_PORT];
		dcvs->max_threshold =
			fmt->count_actual - fmt->count_min_host + 2;

		dcvs->min_threshold =
			msm_vidc_get_extra_buff_count(inst, dcvs->buffer_type);
	} else {
		dprintk(VIDC_ERR, "%s: invalid session type %#x\n",
			__func__, inst->session_type);
		return;
	}

	total_freq = cycles * load;

	for (i = core->resources.allowed_clks_tbl_size - 1; i >= 0; i--) {
		rate = allowed_clks_tbl[i].clock_rate;
		if (rate >= total_freq)
			break;
	}

	dcvs->load = dcvs->load_norm = rate;

	dcvs->load_low = i < (core->resources.allowed_clks_tbl_size - 1) ?
		allowed_clks_tbl[i+1].clock_rate : dcvs->load_norm;
	dcvs->load_high = i > 0 ? allowed_clks_tbl[i-1].clock_rate :
		dcvs->load_norm;

	inst->clk_data.buffer_counter = 0;

	msm_dcvs_print_dcvs_stats(dcvs);

	rc = msm_comm_scale_clocks_and_bus(inst);

	if (rc)
		dprintk(VIDC_ERR, "%s Failed to scale Clocks and Bus\n",
			__func__);
}

int msm_vidc_decide_work_route_iris1(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hfi_video_work_route pdata;
	struct v4l2_format *f;
	u32 codec;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;

	pdata.video_work_route = 2;
	codec = get_v4l2_codec(inst);
	if (inst->session_type == MSM_VIDC_DECODER) {
		switch (codec) {
		case V4L2_PIX_FMT_MPEG2:
			pdata.video_work_route = 1;
			break;
		case V4L2_PIX_FMT_H264:
			if (inst->pic_struct !=
				MSM_VIDC_PIC_STRUCT_PROGRESSIVE)
				pdata.video_work_route = 1;
			break;
		}
	} else if (inst->session_type == MSM_VIDC_ENCODER) {
		u32 slice_mode = 0;
		u32 output_width, output_height, fps, mbps;

		switch (codec) {
		case V4L2_PIX_FMT_VP8:
		case V4L2_PIX_FMT_TME:
			pdata.video_work_route = 1;
			goto decision_done;
		}

		if (inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) {
			pdata.video_work_route = 2;
			goto decision_done;
		}
		slice_mode =  msm_comm_g_ctrl_for_id(inst,
				V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE);
		f = &inst->fmts[OUTPUT_PORT].v4l2_fmt;
		output_height = f->fmt.pix_mp.height;
		output_width = f->fmt.pix_mp.width;
		fps = inst->clk_data.frame_rate >> 16;
		mbps = NUM_MBS_PER_SEC(output_height, output_width, fps);
		if (slice_mode ==
			V4L2_MPEG_VIDEO_MULTI_SICE_MODE_MAX_BYTES ||
			(inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR &&
			mbps <= CBR_MB_LIMIT) ||
			(inst->rc_type ==
				V4L2_MPEG_VIDEO_BITRATE_MODE_CBR_VFR &&
			mbps <= CBR_VFR_MB_LIMIT)) {
			pdata.video_work_route = 1;
			dprintk(VIDC_HIGH, "Configured work route = 1");
		}
	} else {
		return -EINVAL;
	}

decision_done:

	inst->clk_data.work_route = pdata.video_work_route;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HFI_PROPERTY_PARAM_WORK_ROUTE,
			(void *)&pdata, sizeof(pdata));
	if (rc)
		dprintk(VIDC_ERR,
			" Failed to configure work route %pK\n", inst);

	return rc;
}

int msm_vidc_decide_work_route_iris2(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hfi_video_work_route pdata;
	bool is_legacy_cbr;
	u32 codec;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;
	is_legacy_cbr = inst->clk_data.is_legacy_cbr;
	pdata.video_work_route = 4;

	codec  = get_v4l2_codec(inst);
	if (inst->session_type == MSM_VIDC_DECODER) {
		if (codec == V4L2_PIX_FMT_MPEG2 ||
			inst->pic_struct != MSM_VIDC_PIC_STRUCT_PROGRESSIVE)
			pdata.video_work_route = 1;
	} else if (inst->session_type == MSM_VIDC_ENCODER) {
		u32 slice_mode, width, height;
		struct v4l2_format *f;

		slice_mode =  msm_comm_g_ctrl_for_id(inst,
				V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE);
		f = &inst->fmts[INPUT_PORT].v4l2_fmt;
		height = f->fmt.pix_mp.height;
		width = f->fmt.pix_mp.width;

		if (slice_mode == V4L2_MPEG_VIDEO_MULTI_SICE_MODE_MAX_BYTES ||
			codec == V4L2_PIX_FMT_VP8 || is_legacy_cbr) {
			pdata.video_work_route = 1;
		}
	} else {
		return -EINVAL;
	}

	dprintk(VIDC_HIGH, "Configurng work route = %u",
			pdata.video_work_route);

	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HFI_PROPERTY_PARAM_WORK_ROUTE,
			(void *)&pdata, sizeof(pdata));
	if (rc)
		dprintk(VIDC_ERR,
			" Failed to configure work route %pK\n", inst);
	else
		inst->clk_data.work_route = pdata.video_work_route;

	return rc;
}

static int msm_vidc_decide_work_mode_ar50(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hfi_video_work_mode pdata;
	struct hfi_enable latency;
	struct v4l2_format *f;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;
	if (inst->clk_data.low_latency_mode) {
		pdata.video_work_mode = HFI_WORKMODE_1;
		goto decision_done;
	}

	f = &inst->fmts[INPUT_PORT].v4l2_fmt;
	if (inst->session_type == MSM_VIDC_DECODER) {
		pdata.video_work_mode = HFI_WORKMODE_2;
		switch (f->fmt.pix_mp.pixelformat) {
		case V4L2_PIX_FMT_MPEG2:
			pdata.video_work_mode = HFI_WORKMODE_1;
			break;
		case V4L2_PIX_FMT_H264:
		case V4L2_PIX_FMT_HEVC:
			if (f->fmt.pix_mp.height *
				f->fmt.pix_mp.width <= 1280 * 720)
				pdata.video_work_mode = HFI_WORKMODE_1;
			break;
		}
	} else if (inst->session_type == MSM_VIDC_ENCODER)
		pdata.video_work_mode = HFI_WORKMODE_1;
	else {
		return -EINVAL;
	}

decision_done:

	inst->clk_data.work_mode = pdata.video_work_mode;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HFI_PROPERTY_PARAM_WORK_MODE,
			(void *)&pdata, sizeof(pdata));
	if (rc)
		dprintk(VIDC_ERR,
				" Failed to configure Work Mode %pK\n", inst);

	/* For WORK_MODE_1, set Low Latency mode by default to HW. */

	if (inst->session_type == MSM_VIDC_ENCODER &&
			inst->clk_data.work_mode == HFI_WORKMODE_1) {
		latency.enable = true;
		rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session,
			HFI_PROPERTY_PARAM_VENC_LOW_LATENCY_MODE,
			(void *)&latency, sizeof(latency));
	}

	rc = msm_comm_scale_clocks_and_bus(inst);

	return rc;
}

int msm_vidc_decide_work_mode_iris1(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hfi_video_work_mode pdata;
	struct hfi_enable latency;
	u32 yuv_size = 0;
	struct v4l2_format *f;
	u32 codec;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;

	if (inst->clk_data.low_latency_mode) {
		pdata.video_work_mode = HFI_WORKMODE_1;
		dprintk(VIDC_HIGH, "Configured work mode = 1");
		goto decision_done;
	}

	codec = get_v4l2_codec(inst);
	if (inst->session_type == MSM_VIDC_DECODER) {
		f = &inst->fmts[INPUT_PORT].v4l2_fmt;
		pdata.video_work_mode = HFI_WORKMODE_2;
		switch (codec) {
		case V4L2_PIX_FMT_MPEG2:
			pdata.video_work_mode = HFI_WORKMODE_1;
			break;
		case V4L2_PIX_FMT_H264:
		case V4L2_PIX_FMT_HEVC:
		case V4L2_PIX_FMT_VP8:
		case V4L2_PIX_FMT_VP9:
			yuv_size = f->fmt.pix_mp.height * f->fmt.pix_mp.width;
			if ((inst->pic_struct !=
				 MSM_VIDC_PIC_STRUCT_PROGRESSIVE) ||
				(yuv_size  <= 1280 * 720))
				pdata.video_work_mode = HFI_WORKMODE_1;
			break;
		}
	} else if (inst->session_type == MSM_VIDC_ENCODER) {
		pdata.video_work_mode = HFI_WORKMODE_2;

		switch (codec) {
		case V4L2_PIX_FMT_VP8:
		case V4L2_PIX_FMT_TME:
			pdata.video_work_mode = HFI_WORKMODE_1;
			goto decision_done;
		}

	} else {
		return -EINVAL;
	}

decision_done:

	inst->clk_data.work_mode = pdata.video_work_mode;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HFI_PROPERTY_PARAM_WORK_MODE,
			(void *)&pdata, sizeof(pdata));
	if (rc)
		dprintk(VIDC_ERR,
			" Failed to configure Work Mode %pK\n", inst);

	/* For WORK_MODE_1, set Low Latency mode by default to HW. */

	if (inst->session_type == MSM_VIDC_ENCODER &&
			inst->clk_data.work_mode == HFI_WORKMODE_1) {
		latency.enable = true;
		rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session,
			HFI_PROPERTY_PARAM_VENC_LOW_LATENCY_MODE,
			(void *)&latency, sizeof(latency));
	}

	return rc;
}

int msm_vidc_decide_work_mode_iris2(struct msm_vidc_inst *inst)
{
	int rc = 0;
	struct hfi_device *hdev;
	struct hfi_video_work_mode pdata;
	struct hfi_enable latency;
	u32 width, height;
	bool res_ok = false;
	struct v4l2_format *out_f;
	struct v4l2_format *inp_f;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	hdev = inst->core->device;
	pdata.video_work_mode = HFI_WORKMODE_2;
	latency.enable = inst->clk_data.low_latency_mode;
	out_f = &inst->fmts[OUTPUT_PORT].v4l2_fmt;
	inp_f = &inst->fmts[INPUT_PORT].v4l2_fmt;
	if (inst->session_type == MSM_VIDC_DECODER) {
		height = out_f->fmt.pix_mp.height;
		width = out_f->fmt.pix_mp.width;
		res_ok = res_is_less_than(width, height, 1280, 720);
		if (inp_f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MPEG2 ||
			inst->pic_struct != MSM_VIDC_PIC_STRUCT_PROGRESSIVE ||
			inst->clk_data.low_latency_mode || res_ok) {
			pdata.video_work_mode = HFI_WORKMODE_1;
		}
	} else if (inst->session_type == MSM_VIDC_ENCODER) {
		height = inp_f->fmt.pix_mp.height;
		width = inp_f->fmt.pix_mp.width;
		res_ok = !res_is_greater_than(width, height, 4096, 2160);
		if (res_ok &&
			(out_f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VP8 ||
			  inst->clk_data.low_latency_mode)) {
			pdata.video_work_mode = HFI_WORKMODE_1;
			/* For WORK_MODE_1, set Low Latency mode by default */
			latency.enable = true;
		}
		if (inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) {
			pdata.video_work_mode = HFI_WORKMODE_1;
			latency.enable = true;
		}
		if (inst->rc_type == RATE_CONTROL_LOSSLESS) {
			pdata.video_work_mode = HFI_WORKMODE_2;
			latency.enable = false;
		}
	} else {
		return -EINVAL;
	}

	dprintk(VIDC_HIGH, "Configuring work mode = %u low latency = %u",
			pdata.video_work_mode,
			latency.enable);

	if (inst->session_type == MSM_VIDC_ENCODER) {
		rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session,
			HFI_PROPERTY_PARAM_VENC_LOW_LATENCY_MODE,
			(void *)&latency, sizeof(latency));
		if (rc)
			dprintk(VIDC_ERR,
				" Failed to configure low latency %pK\n", inst);
		else
			inst->clk_data.low_latency_mode = latency.enable;
	}

	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, HFI_PROPERTY_PARAM_WORK_MODE,
			(void *)&pdata, sizeof(pdata));
	if (rc)
		dprintk(VIDC_ERR,
			" Failed to configure Work Mode %pK\n", inst);
	else
		inst->clk_data.work_mode = pdata.video_work_mode;

	return rc;
}

static inline int msm_vidc_power_save_mode_enable(struct msm_vidc_inst *inst,
	bool enable)
{
	u32 rc = 0;
	u32 prop_id = 0;
	void *pdata = NULL;
	struct hfi_device *hdev = NULL;
	u32 hfi_perf_mode;

	hdev = inst->core->device;
	if (inst->session_type != MSM_VIDC_ENCODER) {
		dprintk(VIDC_LOW,
			"%s : Not an encoder session. Nothing to do\n",
				__func__);
		return 0;
	}

	prop_id = HFI_PROPERTY_CONFIG_VENC_PERF_MODE;
	hfi_perf_mode = enable ? HFI_VENC_PERFMODE_POWER_SAVE :
		HFI_VENC_PERFMODE_MAX_QUALITY;
	pdata = &hfi_perf_mode;
	rc = call_hfi_op(hdev, session_set_property,
			(void *)inst->session, prop_id, pdata,
			sizeof(hfi_perf_mode));
	if (rc) {
		dprintk(VIDC_ERR,
			"%s: Failed to set power save mode for inst: %pK\n",
			__func__, inst);
		return rc;
	}
	inst->flags = enable ?
		inst->flags | VIDC_LOW_POWER :
		inst->flags & ~VIDC_LOW_POWER;

	dprintk(VIDC_HIGH,
		"Power Save Mode for inst: %pK Enable = %d\n", inst, enable);

	return rc;
}

static int msm_vidc_move_core_to_power_save_mode(struct msm_vidc_core *core,
	u32 core_id)
{
	struct msm_vidc_inst *inst = NULL;

	dprintk(VIDC_HIGH, "Core %d : Moving all inst to LP mode\n", core_id);
	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		if (inst->clk_data.core_id == core_id &&
			inst->session_type == MSM_VIDC_ENCODER)
			msm_vidc_power_save_mode_enable(inst, true);
	}
	mutex_unlock(&core->lock);

	return 0;
}

static u32 get_core_load(struct msm_vidc_core *core,
	u32 core_id, bool lp_mode, bool real_time)
{
	struct msm_vidc_inst *inst = NULL;
	u32 current_inst_mbs_per_sec = 0, load = 0;
	bool real_time_mode = false;

	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {
		u32 cycles, lp_cycles;

		real_time_mode = inst->flags & VIDC_REALTIME ? true : false;
		if (!(inst->clk_data.core_id & core_id))
			continue;
		if (real_time_mode != real_time)
			continue;
		if (inst->session_type == MSM_VIDC_DECODER) {
			cycles = lp_cycles = inst->clk_data.entry->vpp_cycles;
		} else if (inst->session_type == MSM_VIDC_ENCODER) {
			lp_mode |= inst->flags & VIDC_LOW_POWER;
			if (inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ)
				lp_mode = false;

			cycles = lp_mode ?
				inst->clk_data.entry->low_power_cycles :
				inst->clk_data.entry->vpp_cycles;
		} else {
			continue;
		}
		current_inst_mbs_per_sec = msm_comm_get_inst_load_per_core(inst,
				LOAD_CALC_NO_QUIRKS);
		load += current_inst_mbs_per_sec * cycles /
			inst->clk_data.work_route;
	}
	mutex_unlock(&core->lock);

	return load;
}

int msm_vidc_decide_core_and_power_mode_iris1(struct msm_vidc_inst *inst)
{
	bool enable = false;
	int rc = 0;
	u32 core_load = 0, core_lp_load = 0;
	u32 cur_inst_load = 0, cur_inst_lp_load = 0;
	u32 mbpf, mbps, max_hq_mbpf, max_hq_mbps;
	unsigned long max_freq, lp_cycles = 0;
	struct msm_vidc_core *core;

	if (!inst || !inst->core || !inst->core->device) {
		dprintk(VIDC_ERR,
			"%s Invalid args: Inst = %pK\n",
			__func__, inst);
		return -EINVAL;
	}

	core = inst->core;
	max_freq = msm_vidc_max_freq(inst->core);
	inst->clk_data.core_id = 0;

	core_load = get_core_load(core, VIDC_CORE_ID_1, false, true);
	core_lp_load = get_core_load(core, VIDC_CORE_ID_1, true, true);

	lp_cycles = inst->session_type == MSM_VIDC_ENCODER ?
			inst->clk_data.entry->low_power_cycles :
			inst->clk_data.entry->vpp_cycles;

	cur_inst_load = (msm_comm_get_inst_load(inst, LOAD_CALC_NO_QUIRKS) *
		inst->clk_data.entry->vpp_cycles)/inst->clk_data.work_route;

	cur_inst_lp_load = (msm_comm_get_inst_load(inst,
		LOAD_CALC_NO_QUIRKS) * lp_cycles)/inst->clk_data.work_route;

	mbpf = msm_vidc_get_mbs_per_frame(inst);
	mbps = mbpf * msm_vidc_get_fps(inst);
	max_hq_mbpf = core->resources.max_hq_mbs_per_frame;
	max_hq_mbps = core->resources.max_hq_mbs_per_sec;

	dprintk(VIDC_HIGH, "Core RT Load = %d LP Load = %d\n",
		 core_load, core_lp_load);
	dprintk(VIDC_HIGH, "Max Load = %lu\n", max_freq);
	dprintk(VIDC_HIGH, "Current Load = %d Current LP Load = %d\n",
		cur_inst_load, cur_inst_lp_load);

	if (inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ &&
		(core_load > max_freq || core_lp_load > max_freq)) {
		dprintk(VIDC_ERR,
			"CQ session - Core cannot support this load\n");
		return -EINVAL;
	}

	if (cur_inst_load + core_load <= max_freq) {
		if (mbpf > max_hq_mbpf || mbps > max_hq_mbps)
			enable = true;
		msm_vidc_power_save_mode_enable(inst, enable);
	} else if (cur_inst_lp_load + core_load <= max_freq) {
		msm_vidc_power_save_mode_enable(inst, true);
	} else if (cur_inst_lp_load + core_lp_load <= max_freq) {
		dprintk(VIDC_HIGH, "Moved all inst's to LP");
		msm_vidc_move_core_to_power_save_mode(core, VIDC_CORE_ID_1);
	} else {
		dprintk(VIDC_ERR, "Core cannot support this load\n");
		return -EINVAL;
	}

	inst->clk_data.core_id = VIDC_CORE_ID_1;
	rc = msm_comm_scale_clocks_and_bus(inst);
	msm_print_core_status(core, VIDC_CORE_ID_1);
	return rc;
}

int msm_vidc_decide_core_and_power_mode_iris2(struct msm_vidc_inst *inst)
{
	u32 mbpf, mbps, max_hq_mbpf, max_hq_mbps;
	bool enable = true;

	inst->clk_data.core_id = VIDC_CORE_ID_1;
	msm_print_core_status(inst->core, VIDC_CORE_ID_1);

	/* Power saving always disabled for CQ and LOSSLESS RC modes. */
	mbpf = msm_vidc_get_mbs_per_frame(inst);
	mbps = mbpf * msm_vidc_get_fps(inst);
	max_hq_mbpf = inst->core->resources.max_hq_mbs_per_frame;
	max_hq_mbps = inst->core->resources.max_hq_mbs_per_sec;

	if (inst->rc_type == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ ||
		inst->rc_type == RATE_CONTROL_LOSSLESS ||
		(mbpf <= max_hq_mbpf && mbps <= max_hq_mbps))
		enable = false;

	return msm_vidc_power_save_mode_enable(inst, enable);
}

void msm_vidc_init_core_clk_ops(struct msm_vidc_core *core)
{
	if (!core)
		return;

	if (core->platform_data->vpu_ver == VPU_VERSION_AR50)
		core->core_ops = &core_ops_ar50;
	else if (core->platform_data->vpu_ver == VPU_VERSION_IRIS1)
		core->core_ops = &core_ops_iris1;
	else
		core->core_ops = &core_ops_iris2;
}

void msm_print_core_status(struct msm_vidc_core *core, u32 core_id)
{
	struct msm_vidc_inst *inst = NULL;
	struct v4l2_format *out_f;
	struct v4l2_format *inp_f;

	dprintk(VIDC_PERF, "Instances running on core %u", core_id);
	mutex_lock(&core->lock);
	list_for_each_entry(inst, &core->instances, list) {

		if ((inst->clk_data.core_id != core_id) &&
			(inst->clk_data.core_id != VIDC_CORE_ID_3))
			continue;
		out_f = &inst->fmts[OUTPUT_PORT].v4l2_fmt;
		inp_f = &inst->fmts[INPUT_PORT].v4l2_fmt;
		dprintk(VIDC_PERF,
			"inst %pK (%4ux%4u) to (%4ux%4u) %3u %s %s %s %s %lu\n",
			inst,
			inp_f->fmt.pix_mp.width,
			inp_f->fmt.pix_mp.height,
			out_f->fmt.pix_mp.width,
			out_f->fmt.pix_mp.height,
			inst->clk_data.frame_rate >> 16,
			inst->session_type == MSM_VIDC_ENCODER ? "ENC" : "DEC",
			inst->clk_data.work_mode == HFI_WORKMODE_1 ?
				"WORK_MODE_1" : "WORK_MODE_2",
			inst->flags & VIDC_LOW_POWER ? "LP" : "HQ",
			inst->flags & VIDC_REALTIME ? "RealTime" : "NonRTime",
			inst->clk_data.min_freq);
	}
	mutex_unlock(&core->lock);
}
