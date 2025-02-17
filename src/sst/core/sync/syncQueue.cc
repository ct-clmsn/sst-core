// Copyright 2009-2021 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2021, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include "sst_config.h"

#include "sst/core/sync/syncQueue.h"

#include "sst/core/event.h"
#include "sst/core/serialization/serializer.h"
#include "sst/core/simulation_impl.h"

namespace SST {

using namespace Core::ThreadSafe;
using namespace Core::Serialization;

SyncQueue::SyncQueue() : ActivityQueue(), buffer(nullptr), buf_size(0) {}

SyncQueue::~SyncQueue() {}

bool
SyncQueue::empty()
{
    std::lock_guard<Spinlock> lock(slock);
    return activities.empty();
}

int
SyncQueue::size()
{
    std::lock_guard<Spinlock> lock(slock);
    return activities.size();
}

void
SyncQueue::insert(Activity* activity)
{
    std::lock_guard<Spinlock> lock(slock);
    activities.push_back(activity);

#if SST_EVENT_PROFILING
    Simulation_impl* sim = Simulation_impl::getSimulation();

    serializer ser;

    ser.start_sizing();

    ser& activity;

    size_t size = ser.size();

    sim->messageXferSize += (uint64_t)size;
#endif
}

Activity*
SyncQueue::pop()
{
    // NEED TO FATAL
    // if ( data.size() == 0 ) return nullptr;
    // std::vector<Activity*>::iterator it = data.begin();
    // Activity* ret_val = (*it);
    // data.erase(it);
    // return ret_val;
    return nullptr;
}

Activity*
SyncQueue::front()
{
    // NEED TO FATAL
    return nullptr;
}

void
SyncQueue::clear()
{
    std::lock_guard<Spinlock> lock(slock);
    activities.clear();
}

char*
SyncQueue::getData()
{
    std::lock_guard<Spinlock> lock(slock);

    serializer ser;

    ser.start_sizing();

    ser& activities;

    size_t size = ser.size();

#if SST_EVENT_PROFILING
    Simulation_impl* sim = Simulation_impl::getSimulation();
    sim->messageXferSize += (uint64_t)size;
#endif

    if ( buf_size < (size + sizeof(SyncQueue::Header)) ) {
        if ( buffer != nullptr ) { delete[] buffer; }

        buf_size = size + sizeof(SyncQueue::Header);
        buffer   = new char[buf_size];
    }

    ser.start_packing(buffer + sizeof(SyncQueue::Header), size);

    ser& activities;

    // Delete all the events
    for ( unsigned int i = 0; i < activities.size(); i++ ) {
        delete activities[i];
    }
    activities.clear();

    // Set the size field in the header
    static_cast<SyncQueue::Header*>(static_cast<void*>(buffer))->buffer_size = size + sizeof(SyncQueue::Header);

    return buffer;
}

} // namespace SST
