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

#ifndef SST_CORE_SERIALIZATION_SERIALIZE_SET_H
#define SST_CORE_SERIALIZATION_SERIALIZE_SET_H

#include "sst/core/serialization/serializer.h"

#include <set>
#include <unordered_set>

namespace SST {
namespace Core {
namespace Serialization {

template <class T>
class serialize<std::set<T>>
{
    typedef std::set<T> Set;

public:
    void operator()(Set& v, serializer& ser)
    {
        typedef typename std::set<T>::iterator iterator;
        switch ( ser.mode() ) {
        case serializer::SIZER:
        {
            size_t size = v.size();
            ser.size(size);
            iterator it, end = v.end();
            for ( it = v.begin(); it != end; ++it ) {
                T& t = const_cast<T&>(*it);
                serialize<T>()(t, ser);
            }
            break;
        }
        case serializer::PACK:
        {
            size_t size = v.size();
            ser.pack(size);
            iterator it, end = v.end();
            for ( it = v.begin(); it != end; ++it ) {
                T& t = const_cast<T&>(*it);
                serialize<T>()(t, ser);
            }
            break;
        }
        case serializer::UNPACK:
        {
            size_t size;
            ser.unpack(size);
            for ( size_t i = 0; i < size; ++i ) {
                T t;
                serialize<T>()(t, ser);
                v.insert(t);
            }
            break;
        }
        }
    }
};

template <class T>
class serialize<std::unordered_set<T>>
{
    typedef std::unordered_set<T> Set;

public:
    void operator()(Set& v, serializer& ser)
    {
        typedef typename std::unordered_set<T>::iterator iterator;
        switch ( ser.mode() ) {
        case serializer::SIZER:
        {
            size_t size = v.size();
            ser.size(size);
            iterator it, end = v.end();
            for ( it = v.begin(); it != end; ++it ) {
                T& t = const_cast<T&>(*it);
                serialize<T>()(t, ser);
            }
            break;
        }
        case serializer::PACK:
        {
            size_t size = v.size();
            ser.pack(size);
            iterator it, end = v.end();
            for ( it = v.begin(); it != end; ++it ) {
                T& t = const_cast<T&>(*it);
                serialize<T>()(t, ser);
            }
            break;
        }
        case serializer::UNPACK:
        {
            size_t size;
            ser.unpack(size);
            for ( size_t i = 0; i < size; ++i ) {
                T t;
                serialize<T>()(t, ser);
                v.insert(t);
            }
            break;
        }
        }
    }
};

} // namespace Serialization
} // namespace Core
} // namespace SST

#endif // SST_CORE_SERIALIZATION_SERIALIZE_SET_H
