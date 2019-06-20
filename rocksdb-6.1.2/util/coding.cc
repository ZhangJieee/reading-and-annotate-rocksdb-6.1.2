//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

#include <algorithm>
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"

namespace rocksdb {

// conversion' conversion from 'type1' to 'type2', possible loss of data
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
/*
ΪʲôҪ�����ͣ�int������ɱ䳤���ͣ�varint���أ���Ϊ�˾����ܵĽ�Լ�洢�ռ䡣
varint��һ�ֽ��յı�ʾ���ֵķ���������һ�������ֽ�����ʾһ�����֣�ֵԽС������ʹ��Խ�ٵ��ֽ���������int32���͵����֣�
һ����Ҫ4���ֽڡ����ǲ���Varint�����ں�С��int32���͵����֣��������1���ֽ�����ʾ����Ȼ���¶��кõ�Ҳ�в��õ�һ�棬
����varint��ʾ������������������Ҫ5���ֽ�����ʾ����ͳ�ƵĽǶ���˵��һ�㲻��������Ϣ�е����ֶ��Ǵ�������˴��������£�
����varint�󣬿����ø�С���ֽ�������ʾ������Ϣ��varint�е�ÿ���ֽڵ����λ��bit�������⺬�壬�����λΪ1����ʾ�������ֽ�
Ҳ��������ֵ�һ���֣������λΪ0���������������7λ��bit������ʾ���֡�7λ�ܱ�ʾ���������127�����С��128�����ֶ�������
һ���ֽڱ�ʾ�����ڵ���128�����֣�����˵300�����������ֽ����ڴ��б�ʾΪ��
��         ��
1010 1100 0000 0010
ʵ�ֹ������£�300�Ķ�����Ϊ100101100��ȡ��7λҲ����010 1100�����ڴ���ֽ��У����ڵڶ����ֽ�Ҳ�����ֵ�һ���֣�����ڴ���ֽ�
�����λΪ1�����������ڴ���ֽ�Ϊ1010 1100��300�ĸ�2λҲ����10�ŵ��ڴ�ĸ��ֽ��У���Ϊ���ֵ����ֽڽ�������˸��ֽڰ������
λ������6λ����0��䣬���������ڴ���ֽ�Ϊ0000 0010����������£�int��Ҫ32λ��varint��һ���ֽڵ����λ��Ϊ��ʶλ�����ԣ�һ��
�ֽ�ֻ�ܴ洢7λ����������ر�󣬿�����Ҫ5���ֽڲ��ܴ�ţ�5*8-5����־λ��>32��������if���ĵ������֧���Ǵ������������
*/ //�ο�https://blog.csdn.net/caoshangpa/article/details/78815940
char* EncodeVarint32(char* dst, uint32_t v) {
  // Operate on characters as unsigneds
  unsigned char* ptr = reinterpret_cast<unsigned char*>(dst);
  static const int B = 128;
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;
    *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

//����p��ȡ��Ӧ��valueֵ��������ı�����̶�Ӧ
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const unsigned char*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

//����p��ȡ��Ӧ��valueֵ��������ı�����̶�Ӧ
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const unsigned char*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

}  // namespace rocksdb
