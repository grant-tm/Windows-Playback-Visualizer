#ifndef _Thread_Safe_Queue_h
#define _Thread_Safe_Queue_h

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>

//****************************************************************************
// Thread Safe Queue
//****************************************************************************
class ThreadSafeQueue {
public:
    void enqueue(const std::vector<float>& sample) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(sample);
        lock.unlock();
        cond_var_.notify_one();
    }

    bool dequeue(std::vector<float>& sample) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && isCapturing) {
            cond_var_.wait(lock);
        }
        if (queue_.empty()) {
            sample.empty();
            return false;
        }
        sample = queue_.front();
        queue_.pop();
        return true;
    }

    bool isEmpty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    int size(){
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::atomic<bool> isCapturing{true};

private:
    mutable std::mutex mutex_;
    std::condition_variable cond_var_;
    std::queue<std::vector<float>> queue_;
};

#endif