
/*********************************************************
 *
 *  Copyright (C) 2014 by Vitaliy Vitsentiy
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *********************************************************/


#ifndef __ctpl_thread_pool_H__
#define __ctpl_thread_pool_H__

#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <exception>
#include <future>
#include <mutex>
#include <boost/lockfree/queue.hpp>


#ifndef _ctplThreadPoolLength_
#define _ctplThreadPoolLength_  100
#endif


// thread pool to run user's functors with signature
//      ret func(int id, other_params)
// where id is the index of the thread that runs the functor
// ret is some return type


namespace ctpl {

    class thread_pool {

    public:

        thread_pool() : q(_ctplThreadPoolLength_) { this->init(); }     // 将lockfree::queue的容量设置为_ctplThreadPoolLength_
        thread_pool(int nThreads, int queueSize = _ctplThreadPoolLength_) : q(queueSize) { this->init(); this->resize(nThreads); }

        // the destructor waits for all the functions in the queue to be finished
        ~thread_pool() {
            this->stop(true);
        }

        // get the number of running threads in the pool
        int size() { return static_cast<int>(this->threads.size()); }

        // number of idle threads
        int n_idle() { return this->nWaiting; }
        std::thread & get_thread(int i) { return *this->threads[i]; }

        // change the number of threads in the pool
        // should be called from one thread, otherwise be careful to not interleave(交叉, 交错), also with this->stop()
        // nThreads must be >= 0
        void resize(int nThreads) {
            if (!this->isStop && !this->isDone) {
                int oldNThreads = static_cast<int>(this->threads.size());     // vector<...>::size_type --> int
                if (oldNThreads <= nThreads) {  // if the number of threads is increased
                    this->threads.resize(nThreads);
                    this->flags.resize(nThreads);

                    for (int i = oldNThreads; i < nThreads; ++i) {    // for new created threads
                        this->flags[i] = std::make_shared<std::atomic<bool>>(false);
                        this->set_thread(i);
                    }
                }
                else {  // the number of threads is decreased
                    for (int i = oldNThreads - 1; i >= nThreads; --i) {
                        *this->flags[i] = true;  // this thread will finish
                        this->threads[i]->detach();
                    }
                    {
                        // stop the detached threads that were waiting
                        std::unique_lock<std::mutex> lock(this->mutex);
                        this->cv.notify_all();
                    }
                    this->threads.resize(nThreads);  // safe to delete because the threads are detached
                    this->flags.resize(nThreads);  // safe to delete because the threads have copies of shared_ptr of the flags, not originals
                }
            }
        }

        // empty the queue
        void clear_queue() {
            std::function<void(int id)> * _f;
            while (this->q.pop(_f))
                delete _f;  // empty the queue
        }

        // pops a functional wraper to the original function
        std::function<void(int)> pop() {
            std::function<void(int id)> * _f = nullptr;
            this->q.pop(_f);    // 从q的队头中移除一个元素，并将其赋值给 _f
            std::unique_ptr<std::function<void(int id)>> func(_f);  // at return, delete the function even if an exception occurred
                                                                   // func销毁时自动释放 _f 所指向的空间
            std::function<void(int)> f;
            if (_f)
                f = *_f;
            return f;       // 返回 _f 对应的副本 f, 与 _f 在内存上无关联
        }


        // wait for all computing threads to finish and stop all threads
        // may be called asyncronously to not pause the calling thread while waiting, 可以异步调用以在等待时不阻塞调用线程
        // if isWait == true, all the functions in the queue are run, otherwise the queue is cleared without running the functions
        void stop(bool isWait = false) {
            if (!isWait) {
                if (this->isStop)
                    return;
                this->isStop = true;
                for (int i = 0, n = this->size(); i < n; ++i) {   // 停止所有线程
                    *this->flags[i] = true;  // command the threads to stop
                }
                this->clear_queue();  // empty the queue
            }
            else {
                if (this->isDone || this->isStop)
                    return;
                this->isDone = true;  // give the waiting threads a command to finish
            }
            {
                std::unique_lock<std::mutex> lock(this->mutex);
                this->cv.notify_all();  // stop all waiting threads
            }
            for (int i = 0; i < static_cast<int>(this->threads.size()); ++i) {  // wait for the computing threads to finish
                if (this->threads[i]->joinable())     // 返回 true 表示线程可以加入，即线程已经启动但尚未被加入(join),在 C++ 中，只有在启动线程后，但尚未调用 join() 或 detach() 的情况下，线程才是可加入的。
                    this->threads[i]->join();
            }
            // if there were no threads in the pool but some functors in the queue, the functors are not deleted by the threads
            // therefore delete them here
            this->clear_queue();
            this->threads.clear();
            this->flags.clear();
        }

        // 向任务队列中推送任务，并返回与任务相关联的future对象, 以获取任务执行的结果
        template<typename F, typename... Rest>
        auto push(F && f, Rest&&... rest) ->std::future<decltype(f(0, rest...))> {    // 尾返回类型推导
            auto pck = std::make_shared<std::packaged_task<decltype(f(0, rest...))(int)>>(
                std::bind(std::forward<F>(f), std::placeholders::_1, std::forward<Rest>(rest)...)  // std::placeholders::_1: 占位符,表示第一个参数，将其绑定到f的第一个参数位置
            );    // packaged_task 将一个bind创建的可调用对象与一个future相关联

            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);
            });     // 创建一个指针_f指向一个lambda表达式
            this->q.push(_f);   // 将指针_f放入任务队列q中

            std::unique_lock<std::mutex> lock(this->mutex);   // 是否可以删除该行？
            this->cv.notify_one();

            return pck->get_future();  // future对象，用来获取任务的结果
        }

        // run the user's function that excepts argument int - id of the running thread. returned value is templatized
        // operator returns std::future, where the user can get the result and rethrow the catched exceptins
        template<typename F>
        auto push(F && f) ->std::future<decltype(f(0))> {
            auto pck = std::make_shared<std::packaged_task<decltype(f(0))(int)>>(std::forward<F>(f));

            auto _f = new std::function<void(int id)>([pck](int id) {
                (*pck)(id);
            });
            this->q.push(_f);

            std::unique_lock<std::mutex> lock(this->mutex);
            this->cv.notify_one();

            return pck->get_future();   
        }


    private:

        // deleted
        thread_pool(const thread_pool &);// = delete;       // copy constructor
        thread_pool(thread_pool &&);// = delete;            // move constructor
        thread_pool & operator=(const thread_pool &);// = delete;     // copy assignment
        thread_pool & operator=(thread_pool &&);// = delete;          // move assignment

        void set_thread(int i) {
            std::shared_ptr<std::atomic<bool>> flag(this->flags[i]);  // a copy of the shared ptr to the flag
            auto f = [this, i, flag/* a copy of the shared ptr to the flag */]() {
                std::atomic<bool> & _flag = *flag;
                std::function<void(int id)> * _f;
                bool isPop = this->q.pop(_f);
                while (true) {
                    while (isPop) {  // if there is anything in the queue, 队列不空
                        std::unique_ptr<std::function<void(int id)>> func(_f);  // at return, delete the function even if an exception occurred
                        (*_f)(i);

                        if (_flag)
                            return;  // the thread is wanted to stop, return even if the queue is not empty yet
                        else
                            isPop = this->q.pop(_f);
                    }

                    // the queue is empty here, wait for the next command
                    std::unique_lock<std::mutex> lock(this->mutex);
                    ++this->nWaiting;
                    this->cv.wait(lock, [this, &_f, &isPop, &_flag](){ isPop = this->q.pop(_f); return isPop || this->isDone || _flag; });
                    --this->nWaiting;

                    if (!isPop)
                        return;  // if the queue is empty and this->isDone == true or *flag then return
                }
            };
            this->threads[i].reset(new std::thread(f));  // compiler may not support std::make_unique()
        }

        void init() { this->nWaiting = 0; this->isStop = false; this->isDone = false; }

        std::vector<std::unique_ptr<std::thread>> threads;
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        mutable boost::lockfree::queue<std::function<void(int id)> *> q;
        std::atomic<bool> isDone;
        std::atomic<bool> isStop;
        std::atomic<int> nWaiting;  // how many threads are waiting

        std::mutex mutex;
        std::condition_variable cv;
    };

}

#endif // __ctpl_thread_pool_H__

