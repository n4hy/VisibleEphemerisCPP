#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace ve {
    class ThreadPool {
    public:
        ThreadPool(size_t threads = std::thread::hardware_concurrency()) : stop(false) {
            for(size_t i = 0; i < threads; ++i)
                workers.emplace_back([this] {
                    for(;;) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                            if(this->stop && this->tasks.empty()) return;
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task();
                    }
                });
        }
        ~ThreadPool() {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                stop = true;
            }
            condition.notify_all();
            for(std::thread &worker: workers) worker.join();
        }
        template<class F>
        void enqueue(F&& f) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                tasks.emplace(std::forward<F>(f));
            }
            condition.notify_one();
        }
    private:
        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex queue_mutex;
        std::condition_variable condition;
        bool stop;
    };
}
