#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace nsyshid
{
	class HIDWorkQueue
	{
	  public:
		using Task = std::function<void()>;
		using ErrorHandler = std::function<void(std::exception_ptr)>;

		explicit HIDWorkQueue(size_t capacity = 128) : m_capacity(capacity) {}
		~HIDWorkQueue()
		{
			Stop();
		}

		void Start()
		{
			std::lock_guard lock(m_mutex);
			m_accepting = true;
		}

		bool Submit(Task task, ErrorHandler error)
		{
			std::lock_guard lock(m_mutex);
			if (!m_accepting || m_jobs.size() >= m_capacity)
				return false;
			if (!m_worker.joinable())
				m_worker = std::thread(&HIDWorkQueue::Run, this);
			m_jobs.push({std::move(task), std::move(error)});
			m_condition.notify_one();
			return true;
		}

		void Stop()
		{
			std::thread worker;
			{
				std::lock_guard lock(m_mutex);
				m_accepting = false;
				m_jobs = {};
				worker = std::move(m_worker);
			}
			m_condition.notify_one();
			if (worker.joinable())
				worker.join();
		}

	  private:
		struct Job
		{
			Task task;
			ErrorHandler error;
		};

		void Run()
		{
			while (true)
			{
				Job job;
				{
					std::unique_lock lock(m_mutex);
					m_condition.wait(lock, [this] { return !m_accepting || !m_jobs.empty(); });
					if (!m_accepting)
						return;
					job = std::move(m_jobs.front());
					m_jobs.pop();
				}
				try
				{
					job.task();
				} catch (...)
				{
					job.error(std::current_exception());
				}
			}
		}

		const size_t m_capacity;
		std::mutex m_mutex;
		std::condition_variable m_condition;
		std::queue<Job> m_jobs;
		std::thread m_worker;
		bool m_accepting = false;
	};
} // namespace nsyshid
