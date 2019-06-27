//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Log format information shared by reader and writer.
// See ../doc/log_format.txt for more detail.

#pragma once
namespace rocksdb {
namespace log {

//rocksdb����־�ļ��зֳ��˴�СΪ32KB������block�飬block��������log record��ɣ�log record�ĸ�ʽΪ��
//  4      2       1        
//CRC32 | LEN | LOG TYPE | DATA

//����һ��logrecord�������Ϊ7�����һ��block��ʣ��ռ�<=6byte����ô�������Ϊ���ַ�����
//���ⳤ��Ϊ7��log record�ǲ������κ��û����ݵġ�
enum RecordType {
  // Zero is reserved for preallocated files
  kZeroType = 0,
  //FULL���ͱ�����log record������������record��
  kFullType = 1,

  //record�������ݺܶ࣬������block�Ŀ��ô�С������Ҫ�ֳɼ���log record��
  //��һ������ΪFIRST���м��ΪMIDDLE�����һ��ΪLAST��
  //���ӣ����ǵ��������е�user records��
  //  A: length 1000
  //  B: length 97270
  //  C: length 8000
  //A��ΪFULL���͵�record�洢�ڵ�һ��block�У�B������ֳ�3��log record���ֱ�洢�ڵ�1��2��3��block�У�
  // For fragments
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,

  // For recycled log files
  kRecyclableFullType = 5,
  kRecyclableFirstType = 6,
  kRecyclableMiddleType = 7,
  kRecyclableLastType = 8,
};
static const int kMaxRecordType = kRecyclableLastType;

//rocksdb����־�ļ��зֳ��˴�СΪ32KB������block�飬block��������log record��ɣ�log record�ĸ�ʽΪ��
//  4      2       1        
//CRC32 | LEN | LOG TYPE | DATA
static const unsigned int kBlockSize = 32768; //32KB

// Header is checksum (4 bytes), length (2 bytes), type (1 byte)
static const int kHeaderSize = 4 + 2 + 1;

// Recyclable header is checksum (4 bytes), length (2 bytes), type (1 byte),
// log number (4 bytes).
static const int kRecyclableHeaderSize = 4 + 2 + 1 + 4;

}  // namespace log
}  // namespace rocksdb
