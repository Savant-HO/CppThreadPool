#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <future>
#include <thread>


const int TASK_MAX_THRSHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 60; //单位：秒
//线程池支持的模式
enum class PoolMode{    //避免枚举名不同，枚举项相同时出现的命名冲突 enum class
    MODE_FIXED,//固定数量的线程，可以不考虑线程安全
    MODE_CACHED, //线程数量可动态增长，考虑线程安全
};

//线程类型
class Thread{
public:
    //线程函数对象类型
    using ThreadFunc = std::function<void(int)>; //using用来创建别名或简化复杂的类型声明，这里定义ThreadFunc为function<void()>的别名
    
    //线程构造
    Thread(ThreadFunc func)
        :func_(func)
        ,threadId_(generateId_++)
    {

    }

    //线程析构
    ~Thread()
    {

    }
    
    //启动线程
    void start()
    {
        std::thread t(func_,threadId_); // 线程对象t  和线程函数func_
        t.detach(); 
    }
    //获取线程id
    int getId() const
    {
        return threadId_;
    }
private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; //保存线程id
};

//线程池类型
class ThreadPool{
public:
    //构造
    ThreadPool()
    : initThreadSize_(0)
    , taskSize_(0)
    , idleThreadSize_(0)
    , curThreadSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRSHHOLD)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
    {}
    //线程池析构
    ~ThreadPool()
    {
    isPoolRunning_ = false;

    //等待线程池里面所有的线程返回      有两种状态： 阻塞 & 正在执行任务中
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock,[&]() -> bool{return threads_.size() == 0;}); //线程获取锁，等待之前进行条件检查，如果不符合条件，线程会
                                                                      //释放锁并进入等待状态。在等待状态中，线程会等待另一个线程调用notify_all()来通知，并且条件表达式为真，此时线程唤醒，线程唤醒后，重新获取锁，并继续执行
    }

    //设置线程池工作模式
    void setMode(PoolMode mode)
    {
        if(checkRunningState())
            return ;
        poolMode_ = mode;
    }

    //设置task任务队列上线阈值
    void setTaskQueMaxThreshHold(int threshhold)
    {
        if(checkRunningState())
            return ;
        taskQueMaxThreshHold_ = threshhold;
    }

    //设置线程池cached模式下线程阈值
    void setThreadSizeThreshHold(int threshhold)
    {
        if(checkRunningState())
            return ;
        if(poolMode_ == PoolMode::MODE_CACHED)
            threadSizeThreshHold_ = threshhold;
    }


    //给线程池提交任务
    //使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        //打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func),std::forward<Args>(args)...)
        );
        std::future<RType> result = task->get_future();

        std::unique_lock<std::mutex> lock(taskQueMtx_);
        //线程通信  等待任务队列有空余
        // while(taskQue_.size() == taskQueMaxThreshHold_)
        // {
        //     notFull_.wait(lock);
        // }
        // notFull_.wait(lock,[&]()->bool { return taskQue_.size() < taskQueMaxThreshHold_;});

        //用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回 wait wait_for wait_until
        if(!notFull_.wait_for(lock,[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_;}))
        {
            //表示notFull_等待1s钟，条件依然没有满足
            std::cerr << "task queue is full , submit task fail." << std::endl;

            auto task = std::make_shared<std::packaged_task<RType()>>(
                []()->RType{return RType();}

            );
            (*task)(); //执行任务
            return task->get_future();
        }
        //如果有空余，把任务放入任务队列中
        // taskQue_.emplace(sp);
        taskQue_.emplace([task](){
            //去执行下面的任务 //解引用后是packed_task类型，加()就直接执行了
            (*task)();
        });
        taskSize_++;
        //因为新放了任务，任务队列不空，在notEmpty_上进行通知,赶快分配线程执行任务
        notEmpty_.notify_all();

        //cached模式 任务处理比较紧急，场景：小而快的任务 耗时任务多的话就需要fixed模式
        // 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来？
        if(poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreshHold_)
        {
            //创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            //启动线程
            threads_[threadId]->start();
            //修改线程个数相关变量
            curThreadSize_++;
            idleThreadSize_++;
        }

        //返回任务的Result对象
        return result;
    } 
    
    //开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        //设置线程池的启动状态
        isPoolRunning_ = true;

        //记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        //创建线程对象
        for(int i = 0 ; i < initThreadSize_; i++){
            //创建thread线程对象的时候，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr))
            // threads_.emplace_back(std::move(ptr));//将当前线程池对象和成员函数threadFunc绑定。move()是将对象的状态或者所有权从一个对象转移到另一个对象，只是转移，没有内存的搬运或内存拷贝。
        }

        //启动所有线程
        for(int i = 0 ; i < initThreadSize_; i++){
            threads_[i]->start(); //需要去执行一个线程函数
            idleThreadSize_++;
        }
    }

    //禁用线程池的拷贝构造函数和拷贝赋值运算符。
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator = (const ThreadPool&) = delete;
private:
    //定义线程函数
    void threadFunc(int threadid)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();
        // std::cout << "begin threadFunc tid:" << std::this_thread::get_id() << std::endl;
        // std:: cout << std::this_thread::get_id() << std::endl;
        // std::cout << "end threadFunc tid:" << std::this_thread::get_id() << std::endl;

        for(;;)
        {
            Task task;
            {
                //先获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);//离开作用域才释放锁

                std::cout << "tid:" << std::this_thread::get_id()
                << "尝试获取任务..." << std::endl;

                //cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程结束回收掉？？超过initThreadSize数量的线程要进行回收
                //当前时间 - 上一次线程执行时间 > 60s
                    //每一秒中返回一次  怎么区分：超时返回？还是有任务待执行返回
                //锁加双重条件
                    while(taskQue_.size() == 0)
                    {
                        //线程池要结束，回收线程资源
                        if(!isPoolRunning_)
                        {
                            threads_.erase(threadid);
                            std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                            exitCond_.notify_all();
                            return ; //线程函数结束，线程结束
                        }
                        if(poolMode_ == PoolMode::MODE_CACHED)
                        {
                            if(std::cv_status::timeout == 
                            notEmpty_.wait_for(lock,std::chrono::seconds(1)))
                            {
                                auto now = std::chrono::high_resolution_clock().now();
                                auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                                if(dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                                {
                                    //开始回收当前线程
                                    //记录线程数量的相关变量值修改
                                    //把线程对象从线程列表容器中删除  没有办法 threadFunc <==> thread对象
                                    //threadid => thread对象 => 删除
                                    threads_.erase(threadid);
                                    curThreadSize_--;
                                    idleThreadSize_--;

                                    std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                                    return ;
                                }
                            }
                        }
                        else
                        {
                            //等待notEmpty条件
                            notEmpty_.wait(lock);
                        }
                        //线程池要结束，回收线程资源
                        // if(!isPoolRunning_)
                        // {
                        //     threads_.erase(threadid);

                        //     std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                        //     exitCond_.notify_all();
                        //     return ;//结束线程函数就是结束当前线程了。

                        // }
                        
                    }
                    //线程池要结束，回收线程资源
                    // if(!isPoolRunning_)
                    // {
                    //     break;
                    // }
                    
                
                idleThreadSize_--;
                //从任务队列中取一个任务处理

                std::cout << "tid:" << std::this_thread::get_id()
                << "获取任务成功..." << std::endl;

                task = taskQue_.front(); 
                taskQue_.pop();
                taskSize_--;

                //如果依然有剩余任务，继续通知其他的线程执行任务
                if(taskQue_.size() > 0){
                    notEmpty_.notify_all();
                }


                //取出一个任务，进行通知，通知可以继续提交生产任务
                notFull_.notify_all();

            }
            //应该把锁释放掉

            //当前线程负责执行这个任务
            if(task!=nullptr){
                // task->run(); //执行任务，把任务的返回值setVal方法给到Result
                // task->exec();
                task(); //执行function<void()>
            }
            lastTime = std::chrono::high_resolution_clock().now(); //更新线程执行完任务的时间
            idleThreadSize_++;
            
        }
    }
    //检查pool的运行状态
    bool checkRunningState() const{
        return isPoolRunning_;
    }
private:
    // std::vector<std::unique_ptr<Thread>> threads_; //线程列表
    std::unordered_map<int,std::unique_ptr<Thread>> threads_; // 线程列表， 这样可以根据threadid删除thread对象
    size_t initThreadSize_; //初始的线程数量
    std::atomic_int curThreadSize_;//记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_;// 记录空闲线程的数量
    int threadSizeThreshHold_;//线程上限阈值

    // Task任务 => 函数对象
    using Task = std::function<void()>;//返回值是void，不带参数的函数对象
    std::queue<Task> taskQue_; //任务队列，这里使用只能指针是因为可以管理动态分配的内存，可以确保任务对象run完后自动释放
    std::atomic_int taskSize_; //任务的数量，确保线程安全，直接用原子变量，不用锁
    int taskQueMaxThreshHold_; //任务队列数量上限阈值

    std::mutex taskQueMtx_; //保证任务队列的线程安全
    std::condition_variable notFull_; //表示任务队列不满
    std::condition_variable notEmpty_; //表示任务队列不空
    std::condition_variable exitCond_; //等到线程资源全部回收

    PoolMode poolMode_;//当前线程池的工作模式

    std::atomic_bool isPoolRunning_;//表示当前线程池的启动状态

    
};

#endif