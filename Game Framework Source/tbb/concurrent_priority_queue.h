/*
    Copyright 2005-2011 Intel Corporation.  All Rights Reserved.

    This file is part of Threading Building Blocks.

    Threading Building Blocks is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    version 2 as published by the Free Software Foundation.

    Threading Building Blocks is distributed in the hope that it will be
    useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Threading Building Blocks; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    As a special exception, you may use this file as part of a free software
    library without restriction.  Specifically, if other files instantiate
    templates or use macros or inline functions from this file, or you compile
    this file and link it with other files to produce an executable, this
    file does not by itself cause the resulting executable to be covered by
    the GNU General Public License.  This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
*/

#ifndef __TBB_concurrent_priority_queue_H
#define __TBB_concurrent_priority_queue_H

#if !TBB_PREVIEW_CONCURRENT_PRIORITY_QUEUE
#error Set TBB_PREVIEW_CONCURRENT_PRIORITY_QUEUE to include concurrent_priority_queue.h
#endif

#include "atomic.h"
#include "cache_aligned_allocator.h"
#include "tbb_exception.h"
#include "tbb_stddef.h"
#include "tbb_profiling.h"
#include "_aggregator_internal.h"
#include <vector>
#include <iterator>
#include <functional>

namespace tbb {
namespace interface5 {

using namespace tbb::internal;

//! Concurrent priority queue
template <typename T, typename Compare=std::less<T>, typename A=cache_aligned_allocator<T> >
class concurrent_priority_queue {
 public:
    //! Element type in the queue.
    typedef T value_type;

    //! Reference type
    typedef T& reference;

    //! Const reference type
    typedef const T& const_reference;

    //! Integral type for representing size of the queue.
    typedef size_t size_type;

    //! Difference type for iterator
    typedef ptrdiff_t difference_type;

    //! Allocator type
    typedef A allocator_type;

    //! Constructs a new concurrent_priority_queue with default capacity
    explicit concurrent_priority_queue(const allocator_type& a = allocator_type()) : mark(0), data(a) {
        my_aggregator.initialize_handler(my_functor_t(this));
    }

    //! Constructs a new concurrent_priority_queue with init_sz capacity
    explicit concurrent_priority_queue(size_type init_capacity, const allocator_type& a = allocator_type()) : mark(0), data(a) {
        data.reserve(init_capacity);
        my_aggregator.initialize_handler(my_functor_t(this));
    }

    //! [begin,end) constructor
    template<typename InputIterator>
    concurrent_priority_queue(InputIterator begin, InputIterator end, const allocator_type& a = allocator_type()) : data(begin, end, a)
    {
        mark = 0;
        my_aggregator.initialize_handler(my_functor_t(this));
        heapify();
    }

    //! Copy constructor
    /** State of this queue may not reflect results of pending
	operations on the copied queue. */
    explicit concurrent_priority_queue(const concurrent_priority_queue& src) : mark(src.mark), data(src.data.begin(), src.data.end(), src.data.get_allocator())
    {
        my_aggregator.initialize_handler(my_functor_t(this));
        heapify();
    }

    concurrent_priority_queue(const concurrent_priority_queue& src, const allocator_type& a) : mark(src.mark), data(src.data.begin(), src.data.end(), a)
    {
        my_aggregator.initialize_handler(my_functor_t(this));
        heapify();
    }

    //! Assignment operator
    /** State of this queue may not reflect results of pending
	operations on the copied queue. */
    concurrent_priority_queue& operator=(const concurrent_priority_queue& src) {
        if (this != &src) {
            std::vector<value_type, allocator_type>(src.data.begin(), src.data.end(), src.data.get_allocator()).swap(data);
            mark = src.mark;
        }
        return *this;
    }

    //! Returns true if empty, false otherwise
    /** Returned value may not reflect results of pending operations. */
    bool empty() const { return data.empty(); }

    //! Returns the current number of elements contained in the queue
    /** Returned value may not reflect results of pending operations. */
    size_type size() const { return data.size(); }

    //! Returns the current capacity (i.e. allocated storage) of the queue
    /** Returned value may not reflect results of pending operations. */
    size_type capacity() const { return data.capacity(); }

    //! Pushes elem onto the queue, increasing capacity of queue if necessary
    void push(const_reference elem) {
        cpq_operation op_data(elem, PUSH_OP);
        my_aggregator.execute(&op_data);
        if (op_data.status == FAILED) // exception thrown
            throw_exception(eid_bad_alloc);
    }

    //! Gets a reference to and removes highest priority element
    /** If a highest priority element was found, sets elem and returns true,
        otherwise returns false. */
    bool try_pop(reference elem) {
        cpq_operation op_data(POP_OP);
        op_data.elem = &elem;
        my_aggregator.execute(&op_data);
        return op_data.status==SUCCEEDED;
    }

    //! If current capacity is less than new_cap, increases capacity to new_cap
    void reserve(size_type new_cap) {
        cpq_operation op_data(RESERVE_OP);
        op_data.sz = new_cap;
        my_aggregator.execute(&op_data);
        if (op_data.status == FAILED) // exception thrown
            throw_exception(eid_bad_alloc);
    }

    //! Clear the queue; not thread-safe
    /** Resets size, effectively emptying queue; does not free space.
        May not clear elements added in pending operations. */
    void clear() {
        data.clear();
        mark = 0;
    }

    //! Shrink queue capacity to current contents; not thread-safe
    void shrink_to_fit() {
        std::vector<value_type, allocator_type>(data.begin(), data.end(), data.get_allocator()).swap(data);
    }

    //! Swap this queue with another; not thread-safe
    void swap(concurrent_priority_queue& q) {
        data.swap(q.data);
        std::swap(mark, q.mark);
    }

    //! Return allocator object
    allocator_type get_allocator() const { return data.get_allocator(); }

 private:
    enum operation_type {INVALID_OP, PUSH_OP, POP_OP, RESERVE_OP};
    enum operation_status { WAIT=0, SUCCEEDED, FAILED };

    class cpq_operation : public aggregated_operation<cpq_operation> {
     public:
        operation_type type;
        union {
            value_type *elem;
            size_type sz;
        };
        cpq_operation(const_reference e, operation_type t) :
            type(t), elem(const_cast<value_type*>(&e)) {}
        cpq_operation(operation_type t) : type(t) {}
    };

    class my_functor_t {
        concurrent_priority_queue<T, Compare, A> *cpq;
     public:
        my_functor_t() {}
        my_functor_t(concurrent_priority_queue<T, Compare, A> *cpq_) : cpq(cpq_) {}
        void operator()(cpq_operation* op_list) {
            cpq->handle_operations(op_list);
        }
    };

    aggregator< my_functor_t, cpq_operation> my_aggregator;
    //! Padding added to avoid false sharing
    char padding1[NFS_MaxLineSize - sizeof(aggregator< my_functor_t, cpq_operation >)];
    //! The point at which unsorted elements begin
    size_type mark;
    Compare compare;
    //! Padding added to avoid false sharing
    char padding2[NFS_MaxLineSize - sizeof(size_type) - sizeof(Compare)];
    //! Storage for the heap of elements in queue, plus unheapified elements
    /** data has the following structure:

         binary unheapified
          heap   elements
        ____|_______|____
        |       |       |
        v       v       v
        [_|...|_|_|...|_| |...| ]
         0       ^       ^       ^
                 |       |       |__capacity
                 |       |__size
                 |__mark
                 

        Thus, data stores the binary heap starting at position 0 through
        mark-1 (it may be empty).  Then there are 0 or more elements 
        that have not yet been inserted into the heap, in positions 
        mark through size-1. */
    std::vector<value_type, allocator_type> data;

    void handle_operations(cpq_operation *op_list) {
        cpq_operation *tmp, *pop_list=NULL;

        __TBB_ASSERT(mark == data.size(), NULL);

        // first pass processes all constant time operations: pushes,
        // tops, some pops. Also reserve.
        while (op_list) {
            // ITT note: &(op_list->status) tag is used to cover accesses to op_list
            // node. This thread is going to handle the operation, and so will acquire it
            // and perform the associated operation w/o triggering a race condition; the
            // thread that created the operation is waiting on the status field, so when
            // this thread is done with the operation, it will perform a
            // store_with_release to give control back to the waiting thread in
            // aggregator::insert_operation.
            call_itt_notify(acquired, &(op_list->status));
            __TBB_ASSERT(op_list->type != INVALID_OP, NULL);
            tmp = op_list;
            op_list = itt_hide_load_word(op_list->next);
            if (tmp->type == PUSH_OP) {
                __TBB_TRY {
                    data.push_back(*(tmp->elem));
                    itt_store_word_with_release(tmp->status, uintptr_t(SUCCEEDED));
                } __TBB_CATCH(...) {
                    itt_store_word_with_release(tmp->status, uintptr_t(FAILED));
                }
            }
            else if (tmp->type == POP_OP) {
                if (mark < data.size() &&
                    compare(data[0], data[data.size()-1])) {
                    // there are newly pushed elems and the last one
                    // is higher than top
                    *(tmp->elem) = data[data.size()-1]; // copy the data
                    itt_store_word_with_release(tmp->status, uintptr_t(SUCCEEDED));
                    data.pop_back();
                    __TBB_ASSERT(mark<=data.size(), NULL);
                }
                else { // no convenient item to pop; postpone
                    itt_hide_store_word(tmp->next, pop_list);
                    pop_list = tmp;
                }
            }
            else {
                __TBB_ASSERT(tmp->type == RESERVE_OP, NULL);
                __TBB_TRY {
                    data.reserve(tmp->sz);
                    itt_store_word_with_release(tmp->status, uintptr_t(SUCCEEDED));
                } __TBB_CATCH(...) {
                    itt_store_word_with_release(tmp->status, uintptr_t(FAILED));
                }
            }
        }

        // second pass processes pop operations
        while (pop_list) {
            tmp = pop_list;
            pop_list = itt_hide_load_word(pop_list->next);
            __TBB_ASSERT(tmp->type == POP_OP, NULL);
            if (data.empty()) {
                itt_store_word_with_release(tmp->status, uintptr_t(FAILED));
            }
            else {
                __TBB_ASSERT(mark<=data.size(), NULL);
                if (mark < data.size() &&
                    compare(data[0], data[data.size()-1])) {
                    // there are newly pushed elems and the last one is
                    // higher than top
                    *(tmp->elem) = data[data.size()-1]; // copy the data
                    itt_store_word_with_release(tmp->status, uintptr_t(SUCCEEDED));
                    data.pop_back();
                }
                else { // extract top and push last element down heap
                    *(tmp->elem) = data[0]; // copy the data
                    itt_store_word_with_release(tmp->status, uintptr_t(SUCCEEDED));
                    reheap();
                }
            }
        }

        // heapify any leftover pushed elements before doing the next
        // batch of operations
        if (mark<data.size()) heapify();
        __TBB_ASSERT(mark == data.size(), NULL);
    }

    //! Merge unsorted elements into heap
    void heapify() {
        if (!mark) mark = 1;
        for (; mark<data.size(); ++mark) {
            // for each unheapified element under size
            size_type cur_pos = mark;
            value_type to_place = data[mark];
            do { // push to_place up the heap
                size_type parent = (cur_pos-1)>>1;
                if (!compare(data[parent], to_place)) break;
                data[cur_pos] = data[parent];
                cur_pos = parent;
            } while( cur_pos );
            data[cur_pos] = to_place;
        }
    }

    //! Re-heapify after an extraction
    /** Re-heapify by pushing last element down the heap from the root. */
    void reheap() {
        size_type cur_pos=0, child=1;

        while (child < mark) {
            size_type target = child;
            if (child+1 < mark && compare(data[child], data[child+1]))
                ++target;
            // target now has the higher priority child
            if (compare(data[target], data[data.size()-1])) break;
            data[cur_pos] = data[target];
            cur_pos = target;
            child = (cur_pos<<1)+1;
        }
        data[cur_pos] = data[data.size()-1];
        data.pop_back();
        if (mark > data.size()) mark = data.size();
    }
};

} // namespace interface5

using interface5::concurrent_priority_queue;

} // namespace tbb

#endif /* __TBB_concurrent_priority_queue_H */
