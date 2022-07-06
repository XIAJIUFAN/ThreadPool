//#pragma once // 避免头文件重复导入，仅在windows下有效
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <thread>
#include <future>

const int taskQueMaxThreshHold = 2;// 4字节整型的上限值
const int threadQueMaxThreshHold = 100;
const int THREAD_MAX_IDLE_TIME = 10;

// 线程池支持的模式
// C++新标准中，为enum提供了class，避免出现定义两个不同枚举类型，但是枚举项相同，外部使用时冲突的问题
enum class PoolMode
{
	MODEL_FIXED,  // fixed模式线程池
	MODEL_CACHED, // cached模式线程池
};

// 线程类型
class Thread
{
public:
	using ThreadFunc = std::function<void(int)>;
	// 构造函数
	Thread(ThreadFunc func)
	:func_(func)
	,threadId_(generateId_++)
	{}

	// 析构函数
	~Thread() = default;

	// 获取线程id
	int getId() 
	{
		return this->threadId_;
	}

	// 启动线程
	void start() 
	{
		// 创建线程对象，线程开始执行；func_时线程函数对象
		// func_函数对象，threadId_函数执行需要传入的参数
		std::thread t(func_, threadId_);
		t.detach(); // t时局部变量，出作用域会被释放，所以设置成分离线程
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_; // 线程id
};
int Thread::generateId_ = 0; // 静态成员变量类外初始化

 
// 线程池类型
class ThreadPool
{
public:

	// 构造函数
	ThreadPool()
	:initThreadSize_(0)
	, taskSize_(0)
	, curThreadSize_(0)
	, idleThreadSize_(0)
	, taskQueMaxThreshHold_(taskQueMaxThreshHold)
	, threadQueMaxThreshHold_(threadQueMaxThreshHold)
	, mode_(PoolMode::MODEL_FIXED)
	, isPoolRunning_(false)
	{}

	// 析构函数
	~ThreadPool() 
	{
		// 修改运行状态(其他成员变量就不用去维护了，容器可以自己析构)
		isPoolRunning_ = false;

		// 等待线程池里面所有的线程返回
		// 现在线程池中的线程可能的状态：阻塞或者正在执行任务中
		std::unique_lock<std::mutex> lock(taskMtx_);
		notEmpty_.notify_all();// 通知所有处于等待状态的线程都跳到阻塞状态
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	// 设置线程池的工作模式
	void setMode(PoolMode mode) 
	{
		if (checkRunningStage())
			return;
		mode_ = mode;
	}

	// 设置任务队列上限阈值
	void setTaskQueMaxThreshHold(int threshhold) 
	{
		if (checkRunningStage())
			return;
		taskQueMaxThreshHold_ = threshhold;
	}

	// 设置线程队列上限阈值
	void setThreadQueMaxThreshHold(int threshhold) 
	{
		if (checkRunningStage())
			return;
		if (mode_ == PoolMode::MODEL_CACHED)
		{
			threadQueMaxThreshHold_ = threshhold;
		}
	}

	// 给线程池提交任务
	// Result submitTask(std::shared_ptr<Task> sp);
	// 可变参模型编程+返回值类型推导
	template<typename Func, typename...Args>
	auto submitTask(Func&& func, Args&&... args)->std::future<decltype(func(args...))> 
	{// 打包任务，放入任务队列
		
		// 获取返回值类型
		using RType = decltype(func(args...));
		// 使用packaged_task打包任务(函数对象)
		// 使用bind去把func的所有参数都绑定值
		// 使用完美类型转发去保持参数的左值/右值特性
		auto task = std::make_shared<std::packaged_task<RType()>>
			(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

		// future机制等同于我们自己实现的Result
		std::future<RType> result = task->get_future();

		// 获取锁
		std::unique_lock<std::mutex> lock(taskMtx_);

		// 用户提交任务，最长不能阻塞超过1s,否则判断提交任务失败，返回
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
		{
			// 表示notFull_等待了1s，条件依然无法满足
			std::cerr << "task queue is full, submit task fail." << std::endl;
			/*
			任务提交失败了，我们也要进行返回，返回对应类型的零值即可
			Rtype()零构造
			*/
			auto task = std::make_shared<std::packaged_task<RType()>>
				([]()->RType{return RType(); });
			std::future<RType> result = task->get_future();
			(*task)();// 注意去执行一下任务
			return result;
		}

		// 如果有空余，把任务放入任务队列

		/*
		如何把打包好的任务放入任务队列taskQue_中?
			要知道任务队列中任务的类型：using Task = std::function<void()>;
			，而我们打包的task它的函数对象支持的是Rtype()
		*/
		taskQue_.emplace(
			[task]() {
				// 在一个void()类型的函数对象中去执行我们打包好的任务！！
				// 中间层的设计思路很巧妙！
				(*task)();
			});
		taskSize_++;

		// 放了新的任务，队列肯定不空，在notEmpty_上进行通知，去分配线程执行任务
		notEmpty_.notify_all();

		// Cached模式下，如果任务的数量大于空闲线程的数量，如果当前线程的数量还没有超过线程数量的上限阈值就去创建新的线程
		if (mode_ == PoolMode::MODEL_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadQueMaxThreshHold_)
		{
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int trhreadId = ptr->getId();
			threads_.emplace(trhreadId, std::move(ptr));

			std::cout << ">>>>create new thread.... " << std::endl;

			//创建新的线程后，一定记得去启动线程啊！！
			threads_[trhreadId]->start();
			// 与线程个数相关的成员变量要进行修改
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
		 
	}

	// 开启线程池,以CPU核心数量作为线程的初始数量
	void start(int initThreadSize = std::thread::hardware_concurrency()) 
	{
		// 修改线程池的工作状态(正在运行)
		isPoolRunning_ = true;

		// 设置初始线程数量
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程
		for (int i = 0; i < initThreadSize_; ++i)
		{
			// C++14提供std::make_unique<指针所指向的对象类型>(对象构造需要传入的参数)
			// std::placeholders参数占位符
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));

			/*
			另一种创建线程方式
			std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool::threadFunc, this)));

			// ptr是左值，emplace_back底层实现中试图调用unique_ptr左值引用的拷贝构造，但是unique_ptr已经将其禁用
			threads_.emplace_back(ptr);
			*/

			// 移动语义，强转为右值，这样可以使用unique_ptr带右值引用的拷贝构造
			int trhreadId = ptr->getId();
			threads_.emplace(trhreadId, std::move(ptr));
		}

		// 启动所有线程
		for (int i = 0; i < initThreadSize_; ++i)
		{
			threads_[i]->start();
			// 线程池启动并且开始创建线程的时候，空闲线程的数量就要增加
			idleThreadSize_++;
		}
	}

	// 禁止用户去拷贝构造线程池或者进行赋值
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	// 线程函数
	void threadFunc(int threadid) 
	{
		auto lastTime = std::chrono::high_resolution_clock().now();

		// 不断从任务队列中去获取任务
		for (;;)
		{
			Task task;// 默认构造的智能指针，底层的指针为nullptr
			{
				// 获取锁
				std::unique_lock<std::mutex> lock(taskMtx_);

				std::cout<< "tid: " << std::this_thread::get_id()
					<< "尝试获取任务!" << std::endl;

				// 锁+双重判断
				while (taskQue_.size() == 0)
				{
					// 当任务全部执行完后，判断isPoolRunning_的状态，如果为false开始回收线程
					if (!isPoolRunning_)
					{
						// 这里回收执行完任务的线程
						threads_.erase(threadid);
						// 执行时，我们去创建线程看到的是std::this_thread::get_id()这个线程id
						std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
						exitCond_.notify_all(); // 通知线程池析构函数
						return; // 线程函数结束，线程结束
					}

					if (mode_ == PoolMode::MODEL_CACHED)
					{
						/*
						enum class cv_status
						{
							timeout,
							no_timeout
						};
						*/
						// 每等待1s钟都会返回一次，根据wait_for的返回值区分是超时返回，还是达到notEmpty_状态返回
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							// 计算时间间隔，即线程目前处于空闲状态的时间，使用duration_cast可以将表示时间间隔的单位转换成我们指定的类型，这里以s为单位
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// 空闲超时，准备回收当前线程
								// 记录线程数量的相关变量的值的修改
								// 把线程对象从线程列表容器中删除，这里有一个问题：如果线程池中使用vector来存储线程，那么回收是怎么把线程和threadfunc对应起来？
								// 为了能将thread和threadFunc对应起来，以回收线程，我们把线程列表容器由vector-->unordered_map，存储键值对,key值为线程id
								threads_.erase(threadid); // 这个threadid是便于我们进行回收
								curThreadSize_--;
								idleThreadSize_--;

								// 执行时，我们去创建线程看到的是std::this_thread::get_id()这个线程id
								std::cout << "threadid: " << std::this_thread::get_id() << " exit!" << std::endl;
								return;
							}
						}
					}
					else
					{// FIXED模式

						notEmpty_.wait(lock);
					}
				}


				// 如果队列不空，从任务队列中取一个任务	 
				// auto ptask = taskQue_.front();
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				idleThreadSize_--; // 分配线程去执行任务，空闲线程减1

				std::cout << "tid: " << std::this_thread::get_id()
					<< "获取任务成功!" << std::endl;

				// 取出一个任务后，任务队列不为空，通知其他线程去执行任务
				// 两个条件变量，可以实现像这样更加精细的操作
				if (taskQue_.size() > 0)
				{
					notEmpty_.notify_all();
				}

				// 通知现在任务队列不满，可以继续生产（提交）任务了
				notFull_.notify_all();

			}// 出作用域,锁释放

			task();

			lastTime = std::chrono::high_resolution_clock().now();
			idleThreadSize_++; //任务执行完，线程又空闲了
		}

	}

	// 检查pool的运行状态
	bool checkRunningStage() const 
	{
		return this->isPoolRunning_;
	}

private:
	// std::vector<std::unique_ptr<Thread>> threads_;	 
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

	int initThreadSize_; // 初始的线程数量		
	std::atomic_uint idleThreadSize_; // 空闲线程的数量
	std::atomic_uint curThreadSize_; // 记录当前线程池中里面线程的总数量
	int threadQueMaxThreshHold_; // 线程数量的上限阈值，针对Cached模式

	// 队列接收强智能指针
	using Task = std::function<void()>;
	std::queue<Task> taskQue_;	// 任务队列
	std::atomic_uint taskSize_; // 任务的数量
	int taskQueMaxThreshHold_; // 任务数量的上限阈值

	std::mutex taskMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等待线程资源回收

	PoolMode mode_; // 当前线程池的工作模式
	std::atomic_bool isPoolRunning_; // 当前线程池的工作状态
};
#endif // !THREADPOOL_H
