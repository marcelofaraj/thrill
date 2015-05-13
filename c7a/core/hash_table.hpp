/*******************************************************************************
 * c7a/core/hash_table.hpp
 *
 * Hash table with support for reduce and partitions.
 *
 * Part of Project c7a.
 *
 *
 * This file has no license. Only Chuck Norris can compile it.
 ******************************************************************************/

#pragma once
#ifndef C7A_CORE_HASH_TABLE_HEADER
#define C7A_CORE_HASH_TABLE_HEADER

#include <map>
#include <iostream>
#include <c7a/common/logger.hpp>
#include <string>
#include <vector>
#include <stdexcept>
//#include <cstddef>

#include "c7a/api/function_traits.hpp"

//TODO:Remove when we have block manager
#include "c7a/data/data_manager.hpp"

namespace c7a {
namespace core {


template <typename KeyExtractor, typename ReduceFunction>
class HashTable
{
    static const bool debug = true;

    using key_t = typename FunctionTraits<KeyExtractor>::result_type;

    using value_t = typename FunctionTraits<ReduceFunction>::result_type;

protected:
    struct hash_result {
        //! which partition number the item belongs to.
        size_t partition_id;
        //! index within the partition's sub-hashtable of this item
        size_t partition_offset;
        //! index within the whole hashtable
        size_t global_index;

        hash_result(key_t v, const HashTable& ht)
        {
            // partition idx
            size_t hashed = std::hash<key_t>()(v);
            partition_offset = hashed % ht.buckets_per_part_;

            // partition id
            partition_id = partition_offset % ht.num_partitions_;

            // global idx
            global_index = partition_offset + partition_id * ht.buckets_per_part_;
        }
    };

    template <typename key_t, typename value_t>
    struct node {
        key_t   key;
        value_t value;
        node    * next;
    };

public:
    // TODO(ms): the BlockEmitter must be a plain template like KeyExtractor.
    HashTable(size_t partition_size, KeyExtractor key_extractor,
              ReduceFunction reduce_function, data::BlockEmitter<value_t> emit)
        : num_partitions_(partition_size),
          key_extractor_(key_extractor),
          reduce_function_(reduce_function),
          emit_(emit)
    {
        if (partition_size > num_buckets_) {
            throw std::invalid_argument("num partitions must be less than num buckets");
        }

        //num_buckets_ = num_partitions_*10; // TODO scale initial bucket size based on num of workers
        buckets_per_part_ = num_buckets_ / num_partitions_;
        items_in_part_ = new int[num_partitions_];
        for (int i = 0; i < num_partitions_; i++) { // TODO: just a tmp fix
            items_in_part_[i] = 0;
        }
    }

    ~HashTable() { }

    /*!
     * Inserts a key/value pair.
     *
     * Optionally, this may be reduce using the reduce function
     * in case the key already exists.
     */
    void Insert(const value_t& p)
    {
        key_t key = key_extractor_(p);

        hash_result h(key, *this);

        LOG << "key: "
            << key
            << " to idx: "
            << h.global_index;

        // TODO(ms): the first nullptr case is identical. remove and use null as
        // sentinel.

        // bucket is empty
        if (array_[h.global_index] == nullptr) {
            LOG << "bucket empty, inserting...";

            node<key_t, value_t>* n = new node<key_t, value_t>;
            n->key = key;
            n->value = p;
            n->next = nullptr;
            array_[h.global_index] = n;

            // increase counter for partition
            items_in_part_[h.partition_id]++;

            // increase total counter
            total_table_size_++;

            // bucket is not empty
        }
        else {
            LOG << "bucket not empty, checking if key already exists...";

            // check if item with same key
            node<key_t, value_t>* curr_node = array_[h.global_index];
            do {
                if (key == curr_node->key) {
                    LOG << "match of key: "
                        << key
                        << " and "
                        << curr_node->key
                        << " ... reducing...";

                    (*curr_node).value = reduce_function_(curr_node->value, p);

                    LOG << "...finished reduce!";

                    break;
                }

                curr_node = curr_node->next;
            } while (curr_node != nullptr);

            // no item found with key
            if (curr_node == nullptr) {
                LOG << "key doesn't exists in bucket, appending...";

                // insert at first pos
                node<key_t, value_t>* n = new node<key_t, value_t>;
                n->key = key;
                n->value = p;
                n->next = array_[h.global_index];
                array_[h.global_index] = n;

                // increase counter for partition
                items_in_part_[h.partition_id]++;
                // increase total counter
                total_table_size_++;

                LOG << "key appendend, metrics updated!";
            }
        }

        // TODO should be externally configureably somehow
        if (total_table_size_ > max_table_size_) {
            LOG << "spilling in progress";
            PopLargestSubtable();
        }
    }

    /*!
     * Returns a vector containing all items belonging to the partition
     * having the most items.
     */
    void PopLargestSubtable()
    {
        // get partition with max size
        int p_size_max = 0;
        int p_idx = 0;
        for (int i = 0; i < num_partitions_; i++) {
            if (items_in_part_[i] > p_size_max) {
                p_size_max = items_in_part_[i];
                p_idx = i;
            }
        }

        LOG << "currMax: "
            << p_size_max
            << " currentIdx: "
            << p_idx
            << " currentIdx*p_size: "
            << p_idx * buckets_per_part_
            << " CurrentIdx*p_size+p_size-1 "
            << p_idx * buckets_per_part_ + buckets_per_part_ - 1;

        // TODO(ms): use iterators instead of indexes. easier code.

        // retrieve items
        for (size_t i = p_idx * buckets_per_part_;
             i != p_idx * buckets_per_part_ + buckets_per_part_; i++)
        {
            if (array_[i] != nullptr) {
                node<key_t, value_t>* curr_node = array_[i];
                do {
                    emit_(curr_node->value);
                    curr_node = curr_node->next;
                } while (curr_node != nullptr);
                array_[i] = nullptr;
            }
        }

        // reset partition specific counter
        items_in_part_[p_idx] = 0;
        // reset total counter
        total_table_size_ -= p_size_max;
    }

    /*!
     * Flushes the HashTable after all elements are inserted.
     */
    void Flush()
    {
        LOG << "Flushing in progress";

        // TODO(ms): this smells like this should be FlushPE(), since same as above.

        // retrieve items
        for (int i = 0; i < num_partitions_; i++) {
            for (int j = i * buckets_per_part_; j <= i * buckets_per_part_ + buckets_per_part_ - 1; j++) {
                if (array_[j] != nullptr) {
                    node<key_t, value_t>* curr_node = array_[j];
                    do {
                        emit_(curr_node->value);
                        curr_node = curr_node->next;
                    } while (curr_node != nullptr);
                    array_[j] = nullptr;
                }
            }

            // set size of partition to 0
            items_in_part_[i] = 0;
        }

        // reset counters
        total_table_size_ = 0;
    }

    /*!
     * Returns the total num of items.
     */
    size_t Size()
    {
        return total_table_size_;
    }

    void Resize()
    {
        // TODO(ms): make sure that keys still map to the SAME pe.

        LOG << "to be implemented";
    }

    // prints content of hash table
    void Print()
    {
        for (int i = 0; i < num_buckets_; i++) {
            if (array_[i] == nullptr) {
                LOG << "bucket "
                    << i
                    << " empty";
            }
            else {
                std::string log = "";

                // check if item with same key
                node<key_t, value_t>* curr_node = array_[i];
                value_t curr_item;
                do {
                    curr_item = curr_node->value;

                    log += "(";
                    log += curr_item.second;
                    log += ") ";

                    curr_node = curr_node->next;
                } while (curr_node != nullptr);

                LOG << "bucket "
                    << i
                    << ": "
                    << log;
            }
        }

        return;
    }

private:
    //! maximum number of elements in whole table, spill largest subtable when
    //! full
    int max_table_size_ = 3;

    int num_partitions_ = 0;             // partition size

    int buckets_per_part_ = 0;           // num buckets per partition

    int* items_in_part_;                 // num items per partition

    int total_table_size_ = 0;           // total sum of items

    static const int num_buckets_ = 100; // bucket size

    KeyExtractor key_extractor_;

    ReduceFunction reduce_function_;

    //TODO:Network-Emitter when it's there (:
    data::BlockEmitter<value_t> emit_;

    node<key_t, value_t>* array_[num_buckets_] = { nullptr }; // TODO: fix this static assignment
};
}
}

#endif // !C7A_CORE_HASH_TABLE_HEADER

/******************************************************************************/
