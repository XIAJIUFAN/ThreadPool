#include<iostream>
#include <thread>
#include <functional>
#include <future>
#include "threadpool.h"

/*
如何让线程池提交任务更加方便？
1.	pool.submitTask(sum1,10,20);
	pool.submitTask(sum1,10,20, 30);

	submitTask:可变参数模板编程

2.我们之前自己造了一个Result以及相关的类型，代码挺多
	C++11 线程库 thread 
		packaged_task
		async
		使用future来代替Result节省线程池代码
*/

int sum1(int a, int b)
{
	std::this_thread::sleep_for(std::chrono::seconds(5));
	return a + b;
}

int main()
{
	ThreadPool pool;
	//pool.setMode(PoolMode::MODEL_CACHED);
	pool.start(2);

	// 模板的实参推演
	std::future<int> r1 =  pool.submitTask(sum1, 1, 2);
	std::future<int> r2 = pool.submitTask(sum1, 2, 3);
	std::future<int> r3 = pool.submitTask([](int b, int e)->int 
		{
			int sum = 0;
			for (int i = b; i < e; ++i) 
			{
				sum += i;
			}
			return sum;
		}, 1, 100);
	std::future<int> r4 = pool.submitTask(sum1, 2, 3);
	std::future<int> r5 = pool.submitTask(sum1, 3, 4);
	
	std::cout << r1.get() << std::endl;
	std::cout << r2.get() << std::endl;
	std::cout << r3.get() << std::endl;
	std::cout << r4.get() << std::endl;
	std::cout << r5.get() << std::endl;
	return 0;

}