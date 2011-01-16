#include "../StdAfx.h"

#ifdef _MSC_VER
#pragma warning (disable : 4244)
#endif

#include "frame_consumer_device.h"

#include "../video_format.h"

#include <common/concurrency/executor.h>
#include <common/utility/timer.h>

#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/circular_buffer.hpp>

namespace caspar { namespace core {
	
struct frame_consumer_device::implementation
{
	static int const MAX_DEPTH = 3;

	timer clock_;
	executor executor_;	

	boost::circular_buffer<safe_ptr<const read_frame>> buffer_;

	std::map<int, std::shared_ptr<frame_consumer>> consumers_; // Valid iterators after erase
	
	const video_format_desc fmt_;

public:
	implementation(const video_format_desc& format_desc) : fmt_(format_desc)
	{		
		executor_.set_capacity(3);
		executor_.start();
	}

	void add(int index, const safe_ptr<frame_consumer>& consumer)
	{		
		executor_.invoke([&]
		{
			if(buffer_.capacity() < consumer->buffer_depth())
				buffer_.set_capacity(consumer->buffer_depth());
			consumers_[index] = consumer;
		});
	}

	void remove(int index)
	{
		executor_.invoke([&]
		{
			auto it = consumers_.find(index);
			if(it != consumers_.end())
				consumers_.erase(it);
		});
	}
			
	void consume(safe_ptr<const read_frame>&& frame)
	{		
		executor_.begin_invoke([=]
		{	
			clock_.tick(1.0/fmt_.fps);

			buffer_.push_back(std::move(frame));

			if(!buffer_.full())
				return;
	
			auto it = consumers_.begin();
			while(it != consumers_.end())
			{
				try
				{
					it->second->send(buffer_[it->second->buffer_depth()-1]);
					++it;
				}
				catch(...)
				{
					CASPAR_LOG_CURRENT_EXCEPTION();
					consumers_.erase(it++);
					CASPAR_LOG(warning) << "Removed consumer from frame_consumer_device.";
				}
			}
		});
	}
};

frame_consumer_device::frame_consumer_device(frame_consumer_device&& other) : impl_(std::move(other.impl_)){}
frame_consumer_device::frame_consumer_device(const video_format_desc& format_desc) : impl_(new implementation(format_desc)){}
void frame_consumer_device::add(int index, const safe_ptr<frame_consumer>& consumer){impl_->add(index, consumer);}
void frame_consumer_device::remove(int index){impl_->remove(index);}
void frame_consumer_device::consume(safe_ptr<const read_frame>&& future_frame) { impl_->consume(std::move(future_frame)); }
}}