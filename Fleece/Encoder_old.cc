//
//  Encoder.cc
//  Fleece
//
//  Created by Jens Alfke on 1/26/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Encoder.hh"
#include "Endian.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>


namespace fleece {

    using namespace internal;

    // root encoder
    encoder::encoder(Writer& out)
    :_parent(NULL),
     _valPtr(NULL),
     _keyPtr(NULL),
     _count(1),
     _out(out),
     _strings(new stringTable),
     _width(0),
     _writingKey(false),
     _blockedOnKey(false)
    { }

    encoder::encoder(encoder &parent, valueType type, uint32_t count, bool wide)
    :_parent(&parent),
     _count(count),
     _out(_parent->_out),
     _strings(_parent->_strings),
     _width(wide ? 4 : 2)
    {
        bool dict = type==kDict;
        _parent->writeArrayOrDict((dict ? kDictTag : kArrayTag), count, wide, this);
        _writingKey = _blockedOnKey = dict;
    }

    void encoder::reset() {
        if (_parent)
            throw "can only reset root encoder";
        _count = 1;
        _out = Writer();
        delete _strings;
        _strings = new stringTable;
    }

    void encoder::end() {
        if (_count > 0)
            throw "not all items were written";
    }

#pragma mark - WRITING VALUES:

    // primitive to write a value
    const void* encoder::writeValue(tags tag, uint8_t *buf, size_t size, bool canInline) {
        if (_count == 0)
            throw "no more space in collection";
        if (_blockedOnKey)
            throw "need a key before this value";

        if (tag < kPointerTagFirst) {
            // Add (non-pointer) tag:
            assert((buf[0] & 0xF0) == 0);
            buf[0] |= tag<<4;
        }

        const void *result;
        if (_parent) {
            const void* valPtr = _writingKey ? _keyPtr : _valPtr;
            if (size <= _width && canInline) {
                // Add directly to parent collection at offset:
                result = valPtr;
                _out.rewrite(valPtr, slice(buf,size));
                if (size < _width) {
                    int32_t zero = 0;
                    _out.rewrite(offsetby(valPtr, size), slice(&zero, _width-size));
                }
            } else {
                // Write to output, then add a pointer in the parent collection:
                if (_out.length() & 1)
                    _out.write("\0", 1);
                if (!makePointer(_out.length(), valPtr))
                    throw "delta too large to write value";
                result = _out.write(buf, size);
            }
            valPtr = offsetby(valPtr, _width);
            if (_writingKey) {
                _keyPtr = valPtr;
                _keyOff += _width;
            } else {
                _valPtr = valPtr;
                _valOff += _width;
            }
        } else {
            // Root element: just write it
            result = _out.write(buf, size);
        }

        if (_writingKey) {
            _writingKey = false;
        } else {
            --_count;
            if (_keyPtr > 0)
                _blockedOnKey = _writingKey = true;
        }
        return result;
    }

    bool encoder::writePointerTo(uint64_t dstOffset) {
        uint8_t buf[_width];
        if (!makePointer(dstOffset, buf))
            return false;
        writeValue(kPointerTagFirst, buf, _width);
        return true;
    }

    bool encoder::makePointer(uint64_t toOffset, const void *dstPtr) {
        const size_t fromPos = _writingKey ? _keyOff : _valOff;
        ssize_t delta = ((ssize_t)toOffset - (ssize_t)fromPos) / 2;
        if (_width == 2) {
            if (delta < -0x4000 || delta >= 0x4000)
                return false;
            *(int16_t*)dstPtr = (int16_t)_enc16(delta);
        } else {
            assert(_width == 4);
            if (delta < -0x40000000 || delta >= 0x40000000)
                return false;
            *(int32_t*)dstPtr = (int32_t)_enc32(delta);
        }
        *(uint8_t*)dstPtr |= 0x80;  // tag it
        return true;
    }

#pragma mark - SCALARS:

    inline void encoder::writeSpecial(uint8_t special) {
        assert(special <= 0x0F);
        uint8_t buf[2] = {special, 0};
        writeValue(internal::kSpecialTag, buf, 2);
    }

    void encoder::writeNull() {
        writeSpecial(internal::kSpecialValueNull);
    }

    void encoder::writeBool(bool b) {
        writeSpecial(b ? internal::kSpecialValueTrue : internal::kSpecialValueFalse);
    }

    void encoder::writeInt(uint64_t i, bool isShort, bool isUnsigned) {
        if (isShort) {
            uint8_t buf[2] = {(uint8_t)((i >> 8) & 0x0F),
                              (uint8_t)(i & 0xFF)};
            writeValue(internal::kShortIntTag, buf, 2);
        } else {
            uint8_t buf[10];
            size_t size = PutIntOfLength(&buf[1], i, isUnsigned);
            buf[0] = (uint8_t)size - 1;
            if (isUnsigned)
                buf[0] |= 0x08;
            ++size;
            if (size & 1)
                buf[size++] = 0;  // pad to even size
            writeValue(internal::kIntTag, buf, size);
        }
    }

    void encoder::writeInt(int64_t i)   {writeInt(i, (i >= -2048 && i < 2048), false);}
    void encoder::writeUInt(uint64_t i) {writeInt(i, i < 2048, true);}

    void encoder::writeDouble(double n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int64_t)n) {
            return writeInt((int64_t)n);
        } else {
            littleEndianDouble swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = 0x08; // 'double' size flag
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(internal::kFloatTag, buf, sizeof(buf));
        }
    }

    void encoder::writeFloat(float n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int32_t)n) {
            return writeInt((int32_t)n);
        } else {
            littleEndianFloat swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = 0x00; // 'float' size flag
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(internal::kFloatTag, buf, sizeof(buf));
        }
    }

#pragma mark - STRINGS / DATA:

    // used for strings and binary data
    slice encoder::writeData(tags tag, slice s) {
        uint8_t buf[4 + kMaxVarintLen64];
        buf[0] = (uint8_t)std::min(s.size, (size_t)0xF);
        const void *dst;
        if (s.size < _width) {
            // Tiny data fits inline:
            memcpy(&buf[1], s.buf, s.size);
            const void *ptr = writeValue(tag, buf, 1 + s.size, true);
            dst = offsetby(ptr, 1);
        } else {
            // Large data doesn't:
            size_t bufLen = 1;
            if (s.size >= 0x0F)
                bufLen += PutUVarInt(&buf[1], s.size);
            if (s.size == 0)
                buf[bufLen++] = 0;
            writeValue(tag, buf, bufLen, false);
            dst = _out.write(s.buf, s.size);
        }
        return slice(dst, s.size);
    }

    void encoder::writeString(slice s) {
        // Check whether this string's already been written:
        if (s.size >= _width && s.size <= kMaxSharedStringSize) {
            auto entry = _strings->find(s);
            if (entry != _strings->end()) {
                uint64_t offset = entry->second;
                if (writePointerTo(offset))
                    return;
                entry->second = _out.length();      // update with offset of new instance of string
            } else {
                size_t offset = _out.length();
                s = writeData(internal::kStringTag, s);
                //fprintf(stderr, "Caching `%.*s` --> %zu\n", (int)s.size, s.buf, offset);
                _strings->insert({s, offset});
            }
        } else {
            writeData(internal::kStringTag, s);
        }
    }

    void encoder::writeString(std::string s) {
        writeString(slice(s));
    }

    void encoder::writeData(slice s) {
        writeData(internal::kBinaryTag, s);
    }

#pragma mark - ARRAYS / DICTIONARIES:

    void encoder::writeArrayOrDict(internal::tags tag, uint32_t count, bool wide,
                                   encoder *childEncoder) {
        // Write the array/dict header (2 bytes):
        uint8_t buf[2 + kMaxVarintLen32];
        uint32_t inlineCount = std::min(count, (uint32_t)0x07FF);
        buf[0] = (uint8_t)(inlineCount >> 8);
        buf[1] = (uint8_t)(inlineCount & 0xFF);
        size_t bufLen = 2;
        if (count >= 0x0FFF) {
            bufLen += PutUVarInt(&buf[2], count);
            if (bufLen & 1)
                buf[bufLen++] = 0;
        }
        if (wide)
            buf[0] |= 0x08;     // "wide" flag
        writeValue(tag, buf, bufLen, (count==0));          // can inline only if empty

        // Reserve space for the values (and keys, for dicts):
        uint8_t width = (wide ? 4 : 2);
        size_t space = count * width;
        if (tag == internal::kDictTag)
            space *= 2;

        size_t valOff = _out.length(), keyOff = 0;
        const void *valPtr = _out.reserveSpace(space), *keyPtr = NULL;
        if (tag == internal::kDictTag) {
            keyPtr = valPtr;
            keyOff = valOff;
            valPtr = offsetby(valPtr, count*width);
            valOff += count*width;
        }

        childEncoder->_valPtr = valPtr;
        childEncoder->_keyPtr = keyPtr;
        childEncoder->_valOff = valOff;
        childEncoder->_keyOff = keyOff;
    }

    void encoder::writeKey(std::string s)   {writeKey(slice(s));}

    void encoder::writeKey(slice s) {
        if (!_blockedOnKey)
            throw _keyPtr>0 ? "need a value after a key" : "not a dictionary";
        _blockedOnKey = false;
        writeString(s);
    }
/*
    static int keyCmp(void *thunk, const void *a, const void *b) {
        auto self = (encoder*)thunk;
        auto i1 = *(uint32*)a, i2 = *(uint32*)b;
        auto key1 = (const value*)a, key2 = (const value*)b;
        return key1->asString().compare(key2->asString());
    }

    void encoder::sortKeys() {
        uint32_t indexes[_count];
        for (uint32_t i = 0; i < _count; i++)
            indexes[i] = i;
        qsort_r(indexes, _count, sizeof(indexes[0]), this, keyCmp);
    }
*/
}