/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "../../stdafx.h"

#include "image_mixer.h"
#include "image_kernel.h"

#include "../gpu/ogl_device.h"
#include "../gpu/host_buffer.h"
#include "../gpu/device_buffer.h"
#include "../write_frame.h"

#include "../../video_channel_context.h"

#include <common/concurrency/executor.h>
#include <common/exception/exceptions.h>
#include <common/gl/gl_check.h>

#include <core/producer/frame/image_transform.h>
#include <core/producer/frame/pixel_format.h>
#include <core/video_format.h>

#include <gl/glew.h>

#include <boost/foreach.hpp>
#include <boost/range.hpp>
#include <boost/range/algorithm/find.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>

namespace caspar { namespace core {
		
struct image_mixer::implementation : boost::noncopyable
{		
	typedef std::deque<render_item>			layer;

	video_channel_context&					channel_;

	std::vector<image_transform>			transform_stack_;
	std::vector<video_mode::type>			mode_stack_;

	std::deque<std::deque<render_item>>		layers_; // layer/stream/items
	
	image_kernel							kernel_;
		
	std::shared_ptr<device_buffer>			draw_buffer_;

	std::shared_ptr<device_buffer>			local_key_buffer_;
	std::shared_ptr<device_buffer>			layer_key_buffer_;
		
public:
	implementation(video_channel_context& video_channel) 
		: channel_(video_channel)
		, transform_stack_(1)
		, mode_stack_(1, video_mode::progressive)
	{
	}

	~implementation()
	{
		channel_.ogl().gc();
	}
	
	void begin(core::basic_frame& frame)
	{
		transform_stack_.push_back(transform_stack_.back()*frame.get_image_transform());
		mode_stack_.push_back(frame.get_mode() == video_mode::progressive ? mode_stack_.back() : frame.get_mode());
	}
		
	void visit(core::write_frame& frame)
	{	
		CASPAR_ASSERT(!layers_.empty());

		// Check if frame has been discarded by interlacing
		if(boost::range::find(mode_stack_, video_mode::upper) != mode_stack_.end() && boost::range::find(mode_stack_, video_mode::lower) != mode_stack_.end())
			return;
		
		core::render_item item(frame.get_pixel_format_desc(), frame.get_textures(), transform_stack_.back(), mode_stack_.back(), frame.tag());	

		auto& layer = layers_.back();

		if(boost::range::find(layer, item) == layer.end())
			layer.push_back(item);
	}

	void end()
	{
		transform_stack_.pop_back();
		mode_stack_.pop_back();
	}

	void begin_layer()
	{
		layers_.push_back(layer());
	}

	void end_layer()
	{
	}
	
	boost::unique_future<safe_ptr<host_buffer>> render()
	{		
		auto layers = std::move(layers_);
		return channel_.ogl().begin_invoke([=]()mutable
		{
			return render(std::move(layers));
		});
	}
	
	safe_ptr<host_buffer> render(std::deque<layer>&& layers)
	{
		draw_buffer_ = channel_.ogl().create_device_buffer(channel_.get_format_desc().width, channel_.get_format_desc().height, 4);				
		channel_.ogl().clear(*draw_buffer_);
				
		BOOST_FOREACH(auto& layer, layers)
			draw(std::move(layer));
				
		auto host_buffer = channel_.ogl().create_host_buffer(channel_.get_format_desc().size, host_buffer::read_only);
		channel_.ogl().attach(*draw_buffer_);
		host_buffer->begin_read(draw_buffer_->width(), draw_buffer_->height(), format(draw_buffer_->stride()));
		
		GL(glFlush());
		
		return host_buffer;
	}

	void draw(layer&& layer)
	{					
		local_key_buffer_.reset();

		BOOST_FOREACH(auto& item, layer)
			draw(std::move(item));
		
		std::swap(local_key_buffer_, layer_key_buffer_);
	}

	void draw(render_item&& item)
	{											
		if(item.transform.get_is_key())
		{
			if(!local_key_buffer_)
			{
				local_key_buffer_ = channel_.ogl().create_device_buffer(channel_.get_format_desc().width, channel_.get_format_desc().height, 1);
				channel_.ogl().clear(*local_key_buffer_);
			}

			draw(local_key_buffer_, std::move(item), nullptr, nullptr);
		}
		else
		{
			draw(draw_buffer_, std::move(item), local_key_buffer_, layer_key_buffer_);	
			local_key_buffer_.reset();
		}
	}
	
	void draw(std::shared_ptr<device_buffer>& target, render_item&& item, const std::shared_ptr<device_buffer>& local_key, const std::shared_ptr<device_buffer>& layer_key)
	{
		if(!std::all_of(item.textures.begin(), item.textures.end(), std::mem_fn(&device_buffer::ready)))
		{
			CASPAR_LOG(warning) << L"[image_mixer] Performance warning. Host to device transfer not complete, GPU will be stalled";
			channel_.ogl().yield(); // Try to give it some more time.
		}		

		kernel_.draw(channel_.ogl(), std::move(item), make_safe(target), local_key, layer_key);
	}
				
	safe_ptr<write_frame> create_frame(const void* tag, const core::pixel_format_desc& desc)
	{
		return make_safe<write_frame>(channel_.ogl(), tag, desc);
	}
};

image_mixer::image_mixer(video_channel_context& video_channel) : impl_(new implementation(video_channel)){}
void image_mixer::begin(core::basic_frame& frame){impl_->begin(frame);}
void image_mixer::visit(core::write_frame& frame){impl_->visit(frame);}
void image_mixer::end(){impl_->end();}
boost::unique_future<safe_ptr<host_buffer>> image_mixer::render(){return impl_->render();}
safe_ptr<write_frame> image_mixer::create_frame(const void* tag, const core::pixel_format_desc& desc){return impl_->create_frame(tag, desc);}
void image_mixer::begin_layer(){impl_->begin_layer();}
void image_mixer::end_layer(){impl_->end_layer();}
image_mixer& image_mixer::operator=(image_mixer&& other)
{
	impl_ = std::move(other.impl_);
	return *this;
}

}}