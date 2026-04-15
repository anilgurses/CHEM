/* data_pool.hpp
 *
 * Copyright (C) 2022 Anil Gurses
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * @author Anil Gurses <agurses@ncsu.edu>
 *
 */
#pragma once

#include <tbb/concurrent_queue.h>

#include <numa.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "../common.h"

template <typename T, size_t itemSize>
class DataArrayPool {
   public:
    using ptr = std::unique_ptr<T[]>;

    DataArrayPool(size_t poolSize, std::string id, bool zeroOnRelease = false)
        : poolSize(poolSize),
          m_itemSize(itemSize),
          zeroOnRelease(zeroOnRelease),
          _id(id) {
        if (poolSize == 0) {
            throw std::invalid_argument(
                "Pool size must be greater than zero. ID:" + _id);
        }
        for (size_t i = 0; i < poolSize; ++i) {
            pool.push(std::make_unique<T[]>(m_itemSize));
        }
    }

    DataArrayPool(size_t poolSize, size_t runtimeItemSize, std::string id,
                  bool zeroOnRelease = false)
        : poolSize(poolSize),
          m_itemSize(runtimeItemSize),
          zeroOnRelease(zeroOnRelease),
          _id(id) {
        if (poolSize == 0) {
            throw std::invalid_argument(
                "Pool size must be greater than zero. ID:" + _id);
        }
        for (size_t i = 0; i < poolSize; ++i) {
            pool.push(std::make_unique<T[]>(m_itemSize));
        }
    }

    /// NUMA-aware constructor (compile-time item size).
    DataArrayPool(size_t poolSize, std::string id, int numa_node,
                  bool zeroOnRelease = false)
        : poolSize(poolSize),
          m_itemSize(itemSize),
          zeroOnRelease(zeroOnRelease),
          _id(id) {
        if (poolSize == 0) {
            throw std::invalid_argument(
                "Pool size must be greater than zero. ID:" + _id);
        }
        numa_set_preferred(numa_node);
        for (size_t i = 0; i < poolSize; ++i) {
            auto buf = std::make_unique<T[]>(m_itemSize);
            std::memset(buf.get(), 0, m_itemSize * sizeof(T));
            pool.push(std::move(buf));
        }
        numa_set_preferred(-1);
    }

    /// NUMA-aware constructor (runtime item size).
    DataArrayPool(size_t poolSize, size_t runtimeItemSize, std::string id,
                  int numa_node, bool zeroOnRelease = false)
        : poolSize(poolSize),
          m_itemSize(runtimeItemSize),
          zeroOnRelease(zeroOnRelease),
          _id(id) {
        if (poolSize == 0) {
            throw std::invalid_argument(
                "Pool size must be greater than zero. ID:" + _id);
        }
        numa_set_preferred(numa_node);
        for (size_t i = 0; i < poolSize; ++i) {
            auto buf = std::make_unique<T[]>(m_itemSize);
            std::memset(buf.get(), 0, m_itemSize * sizeof(T));
            pool.push(std::move(buf));
        }
        numa_set_preferred(-1);
    }

    ~DataArrayPool() = default;

    ptr acquire() {
        ptr item;
        if (!pool.try_pop(item)) {
            throw std::runtime_error("No available items in the pool. ID:" +
                                     _id);
        }
        return item;
    }

    std::vector<ptr> acquireBatch(size_t n) {
        std::vector<ptr> batch;
        batch.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            ptr item;
            if (!pool.try_pop(item)) {
                // Return already-acquired items before throwing
                for (auto& b : batch) {
                    pool.push(std::move(b));
                }
                throw std::runtime_error(
                    "Not enough items in the pool for batch acquire. ID:" +
                    _id);
            }
            batch.push_back(std::move(item));
        }
        return batch;
    }

    size_t available() { return pool.unsafe_size(); }

    void release(ptr item) {
        if (!item) {
            throw std::invalid_argument("Cannot release a null. ID:" + _id);
        }
        if (zeroOnRelease) {
            std::memset(item.get(), 0, m_itemSize * sizeof(T));
        }
        pool.push(std::move(item));
    }

    size_t getItemSize() const { return m_itemSize; }

   private:
    size_t poolSize;
    size_t m_itemSize;
    bool zeroOnRelease;
    tbb::concurrent_queue<ptr> pool;
    std::string _id;
};

template <typename T>
class DataPool {
   public:
    using ptr = std::unique_ptr<T>;

    DataPool(size_t poolSize, std::string id, bool zeroOnRelease = false)
        : poolSize(poolSize), zeroOnRelease(zeroOnRelease), _id(id) {
        if (poolSize == 0) {
            throw std::invalid_argument(
                "Pool size must be greater than zero. ID:" + _id);
        }
        for (size_t i = 0; i < poolSize; ++i) {
            pool.push(std::make_unique<T>());
        }
    }

    ~DataPool() = default;

    ptr acquire() {
        ptr item;
        if (!pool.try_pop(item)) {
            throw std::runtime_error("No available items in the pool. ID:" +
                                     _id);
        }
        return item;
    }

    size_t available() { return pool.unsafe_size(); }

    void release(ptr item) {
        if (!item) {
            throw std::invalid_argument("Cannot release a null. ID:" + _id);
        }
        if (zeroOnRelease) {
            std::memset(item.get(), 0, sizeof(T));
        }
        pool.push(std::move(item));
    }

   private:
    size_t poolSize;
    bool zeroOnRelease;
    tbb::concurrent_queue<ptr> pool;
    std::string _id;
};

typedef DataArrayPool<char, MTU_SIZE> udpDataPool_t;
typedef DataArrayPool<char, IQ_SIZE> iqPool_t;
typedef std::unique_ptr<char[]> dArray_uptr;
typedef std::shared_ptr<udpDataPool_t> payloadPoolSPtr;
typedef std::shared_ptr<iqPool_t> iqPoolSPtr;
