/*
 * Copyright 2018 Universidad Carlos III de Madrid
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GRPPI_COMMON_MPMC_QUEUE_H
#define GRPPI_COMMON_MPMC_QUEUE_H


#include <vector>
#include <atomic>
#include <iostream>
#include <mutex>
#include <condition_variable>

namespace grppi{

template <typename T>
class atomic_mpmc_queue {
public:

  using value_type = T;

  atomic_mpmc_queue(int size) :
    size_{size},
    buffer_{std::vector<T>(size)}
  {}

  atomic_mpmc_queue(atomic_mpmc_queue && q) noexcept :
    size_{q.size_},
    buffer_{std::move(q.buffer_)},
    pread_{q.pread_.load()},
    pwrite_{q.pwrite_.load()},
    internal_pread_{q.internal_pread_.load()},
    internal_pwrite_{q.internal_pwrite_.load()}
  {}

  atomic_mpmc_queue & operator=(atomic_mpmc_queue && q) noexcept = delete;

  atomic_mpmc_queue(atomic_mpmc_queue const & q) noexcept = delete;
  atomic_mpmc_queue & operator=(atomic_mpmc_queue const & q) noexcept = delete;
  
  bool empty () const noexcept { 
    return pread_.load() == pwrite_.load();
  }

  T pop () noexcept(std::is_nothrow_move_constructible<T>::value);
  void push (T && item) noexcept(std::is_nothrow_move_assignable<T>::value);
  void push (T const & item) noexcept(std::is_nothrow_copy_assignable<T>::value);

private:
  int size_;
  std::vector<T> buffer_;

  std::atomic<unsigned long long> pread_{0};
  std::atomic<unsigned long long> pwrite_{0};
  std::atomic<unsigned long long> internal_pread_{0};
  std::atomic<unsigned long long> internal_pwrite_{0};
};

template <typename T>
T atomic_mpmc_queue<T>::pop() noexcept(std::is_nothrow_move_constructible<T>::value) {
  unsigned long long current;
  do {
    current = internal_pread_.load();
  } while(!internal_pread_.compare_exchange_weak(current, current+1));
          
  while(current >= pwrite_.load());

  auto item = std::move(buffer_[current%size_]); 
  auto aux = current;
  do {
    current = aux;
  } while(!pread_.compare_exchange_weak(current, current+1));
     
  return item;
}

template <typename T>
void atomic_mpmc_queue<T>::push(T && item) noexcept(std::is_nothrow_move_assignable<T>::value) {
  unsigned long long current;
  do{
    current = internal_pwrite_.load();
  } while(!internal_pwrite_.compare_exchange_weak(current, current+1));

  while (current >= (pread_.load()+size_));

  buffer_[current%size_] = std::move(item);
  
  auto aux = current;
  do {
    current = aux;
  } while(!pwrite_.compare_exchange_weak(current, current+1));
}

template <typename T>
void atomic_mpmc_queue<T>::push(T const & item) noexcept(std::is_nothrow_copy_assignable<T>::value) {
  unsigned long long current;
  do{
    current = internal_pwrite_.load();
  } while(!internal_pwrite_.compare_exchange_weak(current, current+1));

  while (current >= (pread_.load()+size_));

  buffer_[current%size_] = item;
  
  auto aux = current;
  do {
    current = aux;
  } while(!pwrite_.compare_exchange_weak(current, current+1));
}

template <typename T>
class locked_mpmc_queue {
public:

  using value_type = T;

  locked_mpmc_queue(int size) :
    size_{size},
    buffer_{std::vector<T>(size)},
    pread_{0},
    pwrite_{0}
  {}

  locked_mpmc_queue(locked_mpmc_queue && q) noexcept :
    size_{q.size_},
    buffer_{std::move(q.buffer_)},
    pread_{q.pread_.load()},
    pwrite_{q.pwrite_.load()}
  {}

  locked_mpmc_queue & operator=(locked_mpmc_queue && q) noexcept = delete;

  locked_mpmc_queue(locked_mpmc_queue const & q) noexcept = delete;
  locked_mpmc_queue & operator=(locked_mpmc_queue const & q) noexcept = delete;
  
  bool empty () const noexcept { 
    return pread_.load() == pwrite_.load();
  }

  T pop () noexcept(std::is_nothrow_move_constructible<T>::value);
  void push (T && item) noexcept(std::is_nothrow_move_assignable<T>::value);
  void push (T const & item) noexcept(std::is_nothrow_copy_assignable<T>::value);

private:
  bool is_full (unsigned long long current) const noexcept;
  bool is_empty (unsigned long long current) const noexcept;

private:
  int size_;
  std::vector<T> buffer_;

  std::atomic<unsigned long long> pread_;
  std::atomic<unsigned long long> pwrite_;

  std::mutex mut_{};
  std::condition_variable empty_{};
  std::condition_variable full_{};
};

template <typename T>
T locked_mpmc_queue<T>::pop() noexcept(std::is_nothrow_move_constructible<T>::value) {
  std::unique_lock<std::mutex> lk(mut_);
  while(pread_.load() >= pwrite_.load()) {
    empty_.wait(lk);
  }  
  auto item = std::move(buffer_[pread_%size_]);
  pread_++;    
  lk.unlock();
  full_.notify_one();
     
  return item;
}

template <typename T>
void locked_mpmc_queue<T>::push(T && item) noexcept(std::is_nothrow_move_assignable<T>::value) {
  std::unique_lock<std::mutex> lk(mut_);
  while (pwrite_.load() >= (pread_.load() + size_)) {
    full_.wait(lk);
  }
  buffer_[pwrite_%size_] = std::move(item);

  pwrite_++;
  lk.unlock();
  empty_.notify_one();
}

template <typename T>
void locked_mpmc_queue<T>::push(T const & item) noexcept(std::is_nothrow_copy_assignable<T>::value) {
  std::unique_lock<std::mutex> lk(mut_);
  while (pwrite_.load() >= (pread_.load() + size_)) {
    full_.wait(lk);
  }
  buffer_[pwrite_%size_] = item;

  pwrite_++;
  lk.unlock();
  empty_.notify_one();
}

enum class queue_mode {lockfree = true, blocking = false};

template <typename T>
class mpmc_queue{
public:
      using value_type = T;

      mpmc_queue<T>(int q_size, queue_mode q_mode ) 
      {
        switch (q_mode) {
          case queue_mode::lockfree:
            new (&buffer_) concrete_queue<atomic_mpmc_queue<T>>(q_size);
            break;
          case queue_mode::blocking:
            new (&buffer_) concrete_queue<atomic_mpmc_queue<T>>(q_size);
            break;
        }
      }

      mpmc_queue(mpmc_queue && q) 
      {
        using concrete_atomic = concrete_queue<atomic_mpmc_queue<T>>;
        using concrete_locked = concrete_queue<locked_mpmc_queue<T>>;

        auto * patomic = dynamic_cast<concrete_atomic*>(q.pself());
        if (patomic) {
          new (&buffer_) concrete_atomic{std::forward<concrete_atomic>(*patomic)};
          return;
        }
        auto * plocked = dynamic_cast<concrete_locked*>(q.pself());
        if (plocked) {
          new (&buffer_) concrete_locked{std::forward<concrete_locked>(*plocked)};
          return;
        }
      }

      mpmc_queue & operator=(mpmc_queue &&) = delete;

      mpmc_queue(const mpmc_queue &) = delete; 
      mpmc_queue & operator=(const mpmc_queue &) = delete;
    
      bool is_empty () const noexcept {
        return pself_const()->empty();
      }

      T pop () {
        return pself()->pop();
      }

      void push(T &&item) {
        pself()->push(std::forward<T>(item));
      }

      void push(T const & x) {
        pself()->push(x);
      }

private:

  struct base_queue {
    virtual ~base_queue() = default;
    virtual bool empty() const noexcept = 0;
    virtual T pop() = 0;
    virtual void push(T &&) = 0;
    virtual void push(const T &) = 0;
  };

  template <typename Q>
  class concrete_queue : public base_queue {
  public:
    concrete_queue(int size) : queue_{size} {}
    concrete_queue(concrete_queue<Q>&&) = default;
    ~concrete_queue() = default;
    virtual bool empty() const noexcept override { return queue_.empty(); }
    virtual T pop() override { return queue_.pop(); }
    virtual void push(T && x) { queue_.push(std::forward<T>(x)); }
    virtual void push(T const & x) { queue_.push(x); }
  private:
    Q queue_;
  };

  base_queue * pself() noexcept {
    return reinterpret_cast<base_queue*>(&buffer_);
  }
      
  base_queue const * pself_const() const noexcept {
    return reinterpret_cast<base_queue const*>(&buffer_);
  }

  std::aligned_union_t<0,
      concrete_queue<atomic_mpmc_queue<T>>,
      concrete_queue<locked_mpmc_queue<T>>> buffer_;
};


namespace internal {

template <typename T>
struct is_queue : std::false_type {};

template <typename T>
struct is_queue<mpmc_queue<T>> : std::true_type {};

}

template <typename T>
constexpr bool is_queue = internal::is_queue<T>();

template <typename T>
using requires_queue = std::enable_if_t<is_queue<T>, int>;

}

#endif
