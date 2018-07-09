/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include "render/buffers.h"
#include "device/device.h"

#include "util/util_debug.h"
#include "util/util_foreach.h"
#include "util/util_hash.h"
#include "util/util_image.h"
#include "util/util_math.h"
#include "util/util_opengl.h"
#include "util/util_time.h"
#include "util/util_types.h"

CCL_NAMESPACE_BEGIN

/* Buffer Params */

BufferParams::BufferParams()
{
	width = 0;
	height = 0;

	full_x = 0;
	full_y = 0;
	full_width = 0;
	full_height = 0;

	aov_color_passes = 0;
	aov_value_passes = 0;
}

void BufferParams::get_offset_stride(int& offset, int& stride)
{
	offset = -(full_x + full_y*width);
	stride = width;
}

bool BufferParams::modified(const BufferParams& params)
{
	return !(full_x == params.full_x
		&& full_y == params.full_y
		&& width == params.width
		&& height == params.height
		&& full_width == params.full_width
		&& full_height == params.full_height
		&& passes.modified(params.passes));
}


/* Render Buffer Task */

RenderTile::RenderTile()
{
	x = 0;
	y = 0;
	w = 0;
	h = 0;

	sample = 0;
	start_sample = 0;
	num_samples = 0;
	resolution = 0;

	offset = 0;
	stride = 0;

	buffer = 0;

	buffers = NULL;
}

/* Render Buffers */

RenderBuffers::RenderBuffers(Device *device)
: buffer(device, "RenderBuffers", MEM_READ_WRITE),
  map_neighbor_copied(false)
{
}

RenderBuffers::~RenderBuffers()
{
	buffer.free();
}

void RenderBuffers::reset(BufferParams& params_)
{
	params = params_;

	/* re-allocate buffer */
	buffer.alloc(params.width*params.height*params.passes.get_size());
	buffer.zero_to_device();
}

void RenderBuffers::zero()
{
	buffer.zero_to_device();
}

bool RenderBuffers::copy_from_device()
{
	if(!buffer.device_pointer)
		return false;

	buffer.copy_from_device(0, params.width * params.passes.get_size(), params.height);

	return true;
}

bool RenderBuffers::get_denoising_pass_rect(int offset, float exposure, int sample, int components, float *pixels)
{
	float invsample = 1.0f/sample;
	float scale = invsample;
	bool variance = (offset == DENOISING_PASS_NORMAL_VAR) ||
	                (offset == DENOISING_PASS_ALBEDO_VAR) ||
	                (offset == DENOISING_PASS_DEPTH_VAR) ||
	                (offset == DENOISING_PASS_COLOR_VAR);

	if(offset == DENOISING_PASS_COLOR) {
		scale *= exposure;
	}
	else if(offset == DENOISING_PASS_COLOR_VAR) {
		scale *= exposure*exposure;
	}

	offset += params.passes.get_denoising_offset();
	int pass_stride = params.passes.get_size();
	int size = params.width*params.height;

	if(variance) {
		/* Approximate variance as E[x^2] - 1/N * (E[x])^2, since online variance
		 * update does not work efficiently with atomics in the kernel. */
		int mean_offset = offset - components;
		float *mean = (float*)buffer.data_pointer + mean_offset;
		float *var = (float*)buffer.data_pointer + offset;
		assert(mean_offset >= 0);

		if(components == 1) {
			for(int i = 0; i < size; i++, mean += pass_stride, var += pass_stride, pixels++) {
				pixels[0] = max(0.0f, var[0] - mean[0]*mean[0]*invsample)*scale;
			}
		}
		else if(components == 3) {
			for(int i = 0; i < size; i++, mean += pass_stride, var += pass_stride, pixels += 3) {
				pixels[0] = max(0.0f, var[0] - mean[0]*mean[0]*invsample)*scale;
				pixels[1] = max(0.0f, var[1] - mean[1]*mean[1]*invsample)*scale;
				pixels[2] = max(0.0f, var[2] - mean[2]*mean[2]*invsample)*scale;
			}
		}
		else {
			return false;
		}
	}
	else {
		float *in = (float*)buffer.data_pointer + offset;

		if(components == 1) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
				pixels[0] = in[0]*scale;
			}
		}
		else if(components == 3) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels += 3) {
				pixels[0] = in[0]*scale;
				pixels[1] = in[1]*scale;
				pixels[2] = in[2]*scale;
			}
		}
		else {
			return false;
		}
	}

	return true;
}

bool RenderBuffers::get_pass_rect(PassType type, float exposure, int sample, int components, float *pixels)
{
	int pass_offset = 0;

	Pass *pass = params.passes.get_pass(type, pass_offset);
	if (!pass) {
		return false;
	}
	
	float *in = (float*)buffer.data_pointer + pass_offset;
	int pass_stride = params.passes.get_size();
	
	float scale = (pass->filter)? 1.0f/(float)sample: 1.0f;
	float scale_exposure = (pass->exposure)? scale*exposure: scale;
	
	int size = params.width*params.height;
	
	if(components == 1) {
		assert(pass->components == components);
		
		/* scalar */
		if(type == PASS_DEPTH) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
				float f = *in;
				pixels[0] = (f == 0.0f)? 1e10f: f*scale_exposure;
			}
		}
		else if(type == PASS_MIST) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
				float f = *in;
				pixels[0] = saturate(f*scale_exposure);
			}
		}
#ifdef WITH_CYCLES_DEBUG
		else if(type == PASS_BVH_TRAVERSED_NODES ||
				type == PASS_BVH_TRAVERSED_INSTANCES ||
				type == PASS_BVH_INTERSECTIONS ||
				type == PASS_RAY_BOUNCES)
		{
			for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
				float f = *in;
				pixels[0] = f*scale;
			}
		}
#endif
		else {
			for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
				float f = *in;
				pixels[0] = f*scale_exposure;
			}
		}
	}
	else if(components == 3) {
		assert(pass->components == 4);
		
		/* RGBA */
		if(type == PASS_SHADOW) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels += 3) {
				float4 f = make_float4(in[0], in[1], in[2], in[3]);
				float invw = (f.w > 0.0f)? 1.0f/f.w: 1.0f;

				pixels[0] = f.x*invw;
				pixels[1] = f.y*invw;
				pixels[2] = f.z*invw;
			}
		}
		else if(pass->divide_type != PASS_NONE) {
			/* RGB lighting passes that need to divide out color */
			params.passes.get_pass(pass->divide_type, pass_offset);
			
			float *in_divide = (float*)buffer.data_pointer + pass_offset;
			
			for(int i = 0; i < size; i++, in += pass_stride, in_divide += pass_stride, pixels += 3) {
				float3 f = make_float3(in[0], in[1], in[2]);
				float3 f_divide = make_float3(in_divide[0], in_divide[1], in_divide[2]);

				f = safe_divide_even_color(f*exposure, f_divide);

				pixels[0] = f.x;
				pixels[1] = f.y;
				pixels[2] = f.z;
			}
		}
		else {
			/* RGB/vector */
			for(int i = 0; i < size; i++, in += pass_stride, pixels += 3) {
				float3 f = make_float3(in[0], in[1], in[2]);

				pixels[0] = f.x*scale_exposure;
				pixels[1] = f.y*scale_exposure;
				pixels[2] = f.z*scale_exposure;
			}
		}
	}
	else if(components == 4) {
		assert(pass->components == components);
		
		/* RGBA */
		if(type == PASS_SHADOW) {
			for(int i = 0; i < size; i++, in += pass_stride, pixels += 4) {
				float4 f = make_float4(in[0], in[1], in[2], in[3]);
				float invw = (f.w > 0.0f)? 1.0f/f.w: 1.0f;

				pixels[0] = f.x*invw;
				pixels[1] = f.y*invw;
				pixels[2] = f.z*invw;
				pixels[3] = 1.0f;
			}
			}
			else if(type == PASS_MOTION) {
			/* need to normalize by number of samples accumulated for motion */
			
			params.passes.get_pass(PASS_MOTION_WEIGHT, pass_offset);
			
			float *in_weight = (float*)buffer.data_pointer + pass_offset;

			for(int i = 0; i < size; i++, in += pass_stride, in_weight += pass_stride, pixels += 4) {
				float4 f = make_float4(in[0], in[1], in[2], in[3]);
				float w = in_weight[0];
				float invw = (w > 0.0f)? 1.0f/w: 0.0f;

				pixels[0] = f.x*invw;
				pixels[1] = f.y*invw;
				pixels[2] = f.z*invw;
				pixels[3] = f.w*invw;
			}
		}
		else {
			for(int i = 0; i < size; i++, in += pass_stride, pixels += 4) {
				float4 f = make_float4(in[0], in[1], in[2], in[3]);

				pixels[0] = f.x*scale_exposure;
				pixels[1] = f.y*scale_exposure;
				pixels[2] = f.z*scale_exposure;

				/* clamp since alpha might be > 1.0 due to russian roulette */
				pixels[3] = saturate(f.w*scale);
			}
		}
	}

	return true;
}

/* Display Buffer */

DisplayBuffer::DisplayBuffer(Device *device, bool linear)
: draw_width(0),
  draw_height(0),
  transparent(true), /* todo: determine from background */
  half_float(linear),
  rgba_byte(device, "display buffer byte"),
  rgba_half(device, "display buffer half")
{
}

DisplayBuffer::~DisplayBuffer()
{
	rgba_byte.free();
	rgba_half.free();
}

void DisplayBuffer::reset(BufferParams& params_)
{
	draw_width = 0;
	draw_height = 0;

	params = params_;

	/* allocate display pixels */
	if(half_float) {
		rgba_half.alloc_to_device(params.width, params.height);
	}
	else {
		rgba_byte.alloc_to_device(params.width, params.height);
	}
}

void DisplayBuffer::draw_set(int width, int height)
{
	assert(width <= params.width && height <= params.height);

	draw_width = width;
	draw_height = height;
}

void DisplayBuffer::draw(Device *device, const DeviceDrawParams& draw_params)
{
	if(draw_width != 0 && draw_height != 0) {
		device_memory& rgba = (half_float)? (device_memory&)rgba_half:
		                                    (device_memory&)rgba_byte;

		device->draw_pixels(rgba, 0, draw_width, draw_height, params.full_x, params.full_y, params.width, params.height, transparent, draw_params);
	}
}

bool DisplayBuffer::draw_ready()
{
	return (draw_width != 0 && draw_height != 0);
}

void DisplayBuffer::write(const string& filename)
{
	int w = draw_width;
	int h = draw_height;

	if(w == 0 || h == 0)
		return;
	
	if(half_float)
		return;

	/* read buffer from device */
	uchar4 *pixels = rgba_byte.copy_from_device(0, w, h);

	/* write image */
	ImageOutput *out = ImageOutput::create(filename);
	ImageSpec spec(w, h, 4, TypeDesc::UINT8);

	out->open(filename, spec);

	/* conversion for different top/bottom convention */
	out->write_image(TypeDesc::UINT8,
		(uchar*)(pixels + (h-1)*w),
		AutoStride,
		-w*sizeof(uchar4),
		AutoStride);

	out->close();

	delete out;
}

bool RenderBuffers::get_aov_rect(ustring name, float exposure, int sample, int components, float *pixels)
{
	int aov_offset = 0;

	AOV *aov = params.passes.get_aov(name, aov_offset);

	if(!aov) {
		return false;
	}

	float *in = (float*)buffer.data_pointer + aov_offset;
	int pass_stride = params.passes.get_size();

	float scale = (aov->type == AOV_RGB) ? exposure / sample : 1.0f / (float)sample; /* TODO has_exposure */

	int size = params.width*params.height;

	switch(components) {
	case 1:
		assert(aov->type == AOV_FLOAT);
		for(int i = 0; i < size; i++, in += pass_stride, pixels++) {
			float f = *in;
			pixels[0] = f * scale;
		}
		break;
	case 3:
		assert(aov->type == AOV_RGB);
		for(int i = 0; i < size; i++, in += pass_stride, pixels += 3) {
			float3 f = make_float3(in[0], in[1], in[2]);

			pixels[0] = f.x*scale;
			pixels[1] = f.y*scale;
			pixels[2] = f.z*scale;
		}
		break;
	case 4:
		assert(aov->type == AOV_CRYPTOMATTE);
		for(int i = 0; i < size; i++, in += pass_stride, pixels += 4) {
			float4 f = make_float4(in[0], in[1], in[2], in[3]);

			/* cryptomatte simple sorting for two layers */
			pixels[0] = f.y > f.w ? f.x : f.z;
			pixels[1] = (f.y > f.w ? f.y : f.w)*scale;
			pixels[2] = f.y > f.w ? f.z : f.x;
			pixels[3] = (f.y > f.w ? f.w : f.y)*scale;
		}
		break;
	default:
		assert(0);
		return false;
	}

	return true;
}

CCL_NAMESPACE_END
