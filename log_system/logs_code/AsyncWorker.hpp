#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

#include "AsyncBuffer.hpp"

#include "Util.hpp"

extern mylog::Util::JsonData* g_conf_data;


namespace mylog {

enum class AsyncType { ASYNC_SAFE, ASYNC_UNSAFE };

using functor = std::function<void(Buffer&)>;

class AsyncWorker {

public:
    using ptr = std::shared_ptr<AsyncWorker>;

    AsyncWorker(const functor& cb, AsyncType async_type = AsyncType::ASYNC_SAFE)
        : async_type_(async_type),
          callback_(cb),
          stop_(false)
    {
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);

        for(size_t i = 0; i < g_conf_data->write_thread_count; ++i) {
            threads_.emplace_back(&AsyncWorker::ThreadEntry, this);
        }
    }

    ~AsyncWorker() { Stop(); }

    void Push(const char* data, size_t len) 
    {
        Node* new_node = new Node();
        new_node->buffer.Push(data, len);
        Node* old_tail = nullptr;
        // 循环直到成功将新节点链接到队尾
        while(true)
        {
            old_tail = tail_.load(std::memory_order_relaxed);
            Node* next = old_tail->next.load(std::memory_order_acquire);
            if(old_tail == tail_.load(std::memory_order_relaxed))// 确保 tail 未被其他线程修改
            {
                if(next == nullptr)// 队尾节点的 next 为空，尝试插入新节点
                {
                    if(old_tail->next.compare_exchange_weak(next, new_node, std::memory_order_release, std::memory_order_relaxed))
                    {
                        tail_.compare_exchange_weak(old_tail, new_node, std::memory_order_release, std::memory_order_relaxed);
                        break;  // 成功添加新节点，退出循环
                    }
                }
                else// 队尾节点的 next 已被其他线程修改，协助推进 tail
                {
                    tail_.compare_exchange_weak(old_tail, next, std::memory_order_release, std::memory_order_relaxed);
                }
            }
        }
    }

    void Stop() 
    {
        stop_ = true;
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();  // 等待所有工作线程结束
            }
        }
    }

private:
    void ThreadEntry() 
    {
        while (1) 
        {
            Node* old_head = nullptr;
            while (true)
            {
                old_head = head_.load(std::memory_order_relaxed);
                Node* next = old_head->next.load(std::memory_order_acquire);
                Node* old_tail = tail_.load(std::memory_order_relaxed);
                if(old_head == head_.load(std::memory_order_relaxed))// 确保 head 未被其他线程修改
                {
                    if (old_head == old_tail)// 队列为空（head == tail）
                    {
                        if (next == nullptr)
                        {
                            break;
                        }
                        // 队尾未更新，协助推进 tail
                        tail_.compare_exchange_weak(old_tail, next, std::memory_order_release, std::memory_order_relaxed);
                    }
                    else// 队列非空，尝试取出头节点数据
                    {
                        if (head_.compare_exchange_weak(old_head, next, std::memory_order_release, std::memory_order_relaxed))
                        {
                            // 删除旧节点
                            callback_(old_head->buffer);  // 处理旧节点的缓冲区数据
                            delete old_head;
                            break;  // 成功删除头结点，退出循环
                        }
                    }
                }
            }
            if(stop_ && head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed)) {
                return;  // 如果工作器停止且队列为空，退出线程
            }
        }
    }

    AsyncType async_type_;
    std::atomic<bool> stop_;  // 用于控制异步工作器的启动

    functor callback_;  // 回调函数，用来告知工作器如何落地

    struct Node
    {
        mylog::Buffer buffer;
        std::atomic<Node*> next;
        Node() : next(nullptr) {}
    };
    std::atomic<Node*> head_;  // 头结点(消费者读取位置)
    std::atomic<Node*> tail_;  // 尾结点(生产者写入位置)
    std::vector<std::thread> threads_;  // 工作线程池
};
}  // namespace mylog