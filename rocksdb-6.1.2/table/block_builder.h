//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#include <vector>

#include <stdint.h>
#include "rocksdb/slice.h"
#include "rocksdb/table.h"
#include "table/data_block_hash_index.h"

namespace rocksdb {
/* һ����¼��KV��ʽ����
//����last key="abcxxxxx"  keyΪ"abcssss"����"abc���Թ���"�� 
//shared=3��key�����"ssss"�ĸ��ַ����Ͳ��ܹ��ã�non_shared=4


// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

*/

//block data�Ĺ����Ƕ�д����ģ���ȡ��ı�����ѯ������Block��ʵ�֣�block data�Ĺ�������BlockBuilder��ʵ��
//ͼ���¼���https://blog.csdn.net/caoshangpa/article/details/78977743
class BlockBuilder {
 public:
  BlockBuilder(const BlockBuilder&) = delete;
  void operator=(const BlockBuilder&) = delete;

  explicit BlockBuilder(int block_restart_interval,
                        bool use_delta_encoding = true,
                        bool use_value_delta_encoding = false,
                        BlockBasedTableOptions::DataBlockIndexType index_type =
                            BlockBasedTableOptions::kDataBlockBinarySearch,
                        double data_block_hash_table_util_ratio = 0.75);

  // Reset the contents as if the BlockBuilder was just constructed.
  void Reset();// ����BlockBuilder

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  
  // Add�ĵ���Ӧ����Reset֮����Finish֮ǰ��
  // Addֻ���KV�ԣ�һ����¼��,��������Ϣ������Finish��ӡ�
  // ÿ�ε���Addʱ��keyӦ��Խ��Խ��
  void Add(const Slice& key, const Slice& value,
           const Slice* const delta_value = nullptr);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // �齨block data��ɣ�����block data
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  // ���㵱ǰblock data�Ĵ�С
  inline size_t CurrentSizeEstimate() const {
    return estimate_ + (data_block_hash_index_builder_.Valid()
                            ? data_block_hash_index_builder_.EstimateSize()
                            : 0);
  }

  // Returns an estimated block size after appending key and value.
  size_t EstimateSizeAfterKV(const Slice& key, const Slice& value) const;

  // Return true iff no entries have been added since the last Reset()
  // �Ƿ��Ѿ���ʼ�齨block data
  bool empty() const { return buffer_.empty(); }

 private:
  //block_restart_interval��ʾ��ǰ�����㣨��ʵҲ��һ����¼�����ϸ�������֮�����˶�������¼
  const int block_restart_interval_;
  // TODO(myabandeh): put it into a separate IndexBlockBuilder
  const bool use_delta_encoding_;
  // Refer to BlockIter::DecodeCurrentValue for format of delta encoded values
  const bool use_value_delta_encoding_;

  /* һ�����⣬��Ȼͨ��Comparator���Լ���Ľ�ʡkey�Ĵ洢�ռ䣬��Ϊʲô��Ҫʹ�����������������ռ��һ�¿ռ��أ�
  ������Ϊ����ͷ�ļ�¼�����𻵣��������м�¼�����޷��ָ���Ϊ�˽���������գ������������㣬ÿ���̶�
  ������¼��ǿ�Ƽ���һ�������㣬���λ�õ�Entry�������ļ�¼�Լ���Key��
  */

  // ���ڴ��block data 
  // block������
  std::string buffer_;              // Destination buffer
  // ���ڴ���������λ����Ϣ  restarts_[i]�洢����block�ĵ�i���������ƫ�ơ�
  //�����Ե�һ��k/v�ԣ����ǵ�һ�������㣬Ҳ����restarts[0] = 0;
  std::vector<uint32_t> restarts_;  // Restart points
  // buffer��С +���������鳤�� + �����㳤��(uint32)  
  size_t estimate_;
  // ���ϸ�������������¸�������ʱ�ļ���
  int counter_;    // Number of entries emitted since restart
  // �Ƿ������Finish
  bool finished_;  // Has Finish() been called?
  // ��¼���Add��key 
  // ��¼�����ӵ�key  
  std::string last_key_;
  DataBlockHashIndexBuilder data_block_hash_index_builder_;
};

}  // namespace rocksdb
