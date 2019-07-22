//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "db/dbformat.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <stdio.h>
#include "monitoring/perf_context_imp.h"
#include "port/port.h"
#include "util/coding.h"
#include "util/string_util.h"

namespace rocksdb {

// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
const ValueType kValueTypeForSeek = kTypeBlobIndex;
const ValueType kValueTypeForSeekForPrev = kTypeDeletion;


/*
�Ƚ�seq����8λ��Ȼ���t���л�������൱�ڰ�t�ŵ���seq�ĵ�8Ϊ��ΪʲôseqҪС�ڵ���kMaxSequenceNumber�ء�


��ΪkMaxSequenceNumber��ֵ������ʾ��
typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
//0x1ull:u-unsigned �޷��ţ�l-long �����ͣ�ll����64λ���͡�����0x1ull����ĺ������޷���64λ���ͳ���1����16���Ʊ�ʾ��
static const SequenceNumber kMaxSequenceNumber = ((0x1ull << 56) - 1);

�ö����Ʊ�ʾ���ǣ�0000 0000 1111 1111 1111 1111 1111 1111�����seq����kMaxSequenceNumber������8λ�Ļ����Ƴ��硣
*/
uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(t));
  return (seq << 8) | t;
}

EntryType GetEntryType(ValueType value_type) {
  switch (value_type) {
    case kTypeValue:
      return kEntryPut;
    case kTypeDeletion:
      return kEntryDelete;
    case kTypeSingleDeletion:
      return kEntrySingleDelete;
    case kTypeMerge:
      return kEntryMerge;
    case kTypeRangeDeletion:
      return kEntryRangeDeletion;
    case kTypeBlobIndex:
      return kEntryBlobIndex;
    default:
      return kEntryOther;
  }
}

bool ParseFullKey(const Slice& internal_key, FullKey* fkey) {
  ParsedInternalKey ikey;
  if (!ParseInternalKey(internal_key, &ikey)) {
    return false;
  }
  fkey->user_key = ikey.user_key;
  fkey->sequence = ikey.sequence;
  fkey->type = GetEntryType(ikey.type);
  return true;
}

void UnPackSequenceAndType(uint64_t packed, uint64_t* seq, ValueType* t) {
  *seq = packed >> 8;
  *t = static_cast<ValueType>(packed & 0xff);

  assert(*seq <= kMaxSequenceNumber);
  assert(IsExtendedValueType(*t));
}

//  Inline bool ParseInternalKey()��internal_key��Slice����������Ϊresult
//  AppendInternalKey() ��key��ParsedInternalKey�����л�Ϊresult��Internel key��

//AppendInternalKey�����Ȱ�user_key��ӵ�*result�У�Ȼ����PackSequenceAndType������sequence��type�������������Ľ����ӵ�*result�С�
void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

void AppendInternalKeyFooter(std::string* result, SequenceNumber s,
                             ValueType t) {
  PutFixed64(result, PackSequenceAndType(s, t));
}

std::string ParsedInternalKey::DebugString(bool hex) const {
  char buf[50];
  snprintf(buf, sizeof(buf), "' seq:%" PRIu64 ", type:%d", sequence,
           static_cast<int>(type));
  std::string result = "'";
  result += user_key.ToString(hex);
  result += buf;
  return result;
}

std::string InternalKey::DebugString(bool hex) const {
  std::string result;
  ParsedInternalKey parsed;
  if (ParseInternalKey(rep_, &parsed)) {
    result = parsed.DebugString(hex);
  } else {
    result = "(bad)";
    result.append(EscapeString(rep_));
  }
  return result;
}

const char* InternalKeyComparator::Name() const { return name_.c_str(); }
/*
��RocksDB�ڲ��������֯�ġ���RocksDB�еĲ�ͬ�汾��key�ǰ���������߼���������:
increasing user key (according to user-supplied comparator)
decreasing sequence number
decreasing type (though sequence# should be enough to disambiguate)
*/
/*
1�����ȱȽ�user_key�����user_key����ͬ����ֱ�ӷ��رȽϽ��������������еڶ�����user_comparator_���û�ָ���ıȽ�������InternalKeyComparator����ʱ���롣
2����user_key��ͬ������£��Ƚ�sequence_numer|value typeȻ�󷵻ؽ��(ע��ÿ��Internal Key��sequence_number��Ψһ�ģ���˲����ܳ���anum==bnum�����)
*/
//���ҵĵط���FindGreaterOrEqual
int InternalKeyComparator::Compare(const ParsedInternalKey& a,
                                   const ParsedInternalKey& b) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)
  int r = user_comparator_.Compare(a.user_key, b.user_key);
  //user key��ͬ���Ƚ�sequence��type
  //��Key��ͬʱ������seq�Ľ������seq��ͬ����type�Ľ���
  if (r == 0) {
    if (a.sequence > b.sequence) {
      r = -1;
    } else if (a.sequence < b.sequence) {
      r = +1;
    } else if (a.type > b.type) {
      r = -1;
    } else if (a.type < b.type) {
      r = +1;
    }
  }
  return r;
}

/*
1���ú���ȡ��Internal Key�е�user_key�ֶΣ������û�ָ����comparator�ҵ����ַ������滻user_start��
��ʱuser_start�������Ǳ���ˣ������߼���ȴ����ˣ����BytewiseComparatorImpl

2�����user_start���滻�ˣ������µ�user_start����Internal Key����ʹ������sequence number������start���ֲ��䡣
*/
void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                  const Slice& limit) const {
  // Attempt to shorten the user portion of the key
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_.FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() <= user_start.size() &&
    user_comparator_.Compare(user_start, tmp) < 0) {
		// if user key�������ϳ��ȱ���ˣ������߼�ֵ�����.�����µ�*startʱ��	
		// ʹ������sequence number���Ա�֤������ͬuser key��¼���еĵ�һ��	
		
	    // User key has become shorter physically, but larger logically.
	    // Tack on the earliest possible number to the shortened user key.
	    PutFixed64(&tmp,
	               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
	    assert(this->Compare(*start, tmp) < 0);
	    assert(this->Compare(tmp, limit) < 0);
	    start->swap(tmp);
  }
}

void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_.FindShortSuccessor(&tmp);
  if (tmp.size() <= user_key.size() &&
      user_comparator_.Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

LookupKey::LookupKey(const Slice& _user_key, SequenceNumber s) {
  size_t usize = _user_key.size();
  size_t needed = usize + 13;  // A conservative estimate
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }

  //start_ָ��size����
  start_ = dst;
  // NOTE: We don't support users keys of more than 2GB :)
  dst = EncodeVarint32(dst, static_cast<uint32_t>(usize + 8));

  //kstart_ָ��_user_key���ݲ���
  kstart_ = dst;
  memcpy(dst, _user_key.data(), usize);
  dst += usize;

  //�����������SequenceNumber+type��type��kValueTypeForSeek
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

void IterKey::EnlargeBuffer(size_t key_size) {
  // If size is smaller than buffer size, continue using current buffer,
  // or the static allocated one, as default
  assert(key_size > buf_size_);
  // Need to enlarge the buffer.
  ResetBuffer();
  buf_ = new char[key_size];
  buf_size_ = key_size;
}
}  // namespace rocksdb
