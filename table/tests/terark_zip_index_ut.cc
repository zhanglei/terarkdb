
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <terark/io/FileStream.hpp>
#include <terark/rank_select.hpp>

#include "../terark_zip_common.h"
#include "../terark_zip_index.h"

using namespace std;

using std::string;
using std::unique_ptr;

using terark::byte_t;
using terark::fstring;
using terark::valvec;
using terark::valvec_no_init;
using terark::valvec_reserve;

using terark::FileStream;
using terark::InputBuffer;
using terark::OutputBuffer;
using terark::LittleEndianDataInput;
using terark::LittleEndianDataOutput;

using rocksdb::TerarkIndex;

namespace rocksdb {
  struct TerarkZipTableOptions {};

  class FileWriter {
  public:
    std::string path;
    FileStream  fp;
    NativeDataOutput<OutputBuffer> writer;
    ~FileWriter() {}
    void open() {
      fp.open(path.c_str(), "wb+");
      fp.disbuf();
      writer.attach(&fp);
    }
    void close() {
      writer.flush_buffer();
      fp.close();
    }
  };
}

/*
 * 1. index1st: uint64, index2nd: uint64
 */
//typedef map<string, int> IndexDict;
//IndexDict dict;
vector<string> keys;
string key_path;
string index_path;
TerarkIndex::KeyStat stat;
void clear() {
  keys.clear();
  key_path.clear();
  index_path.clear();
  memset(&stat, 0, sizeof(stat));
}

void init_data_ascend() {
  rocksdb::FileWriter fwriter;
  fwriter.path = key_path;
  fwriter.open();
  keys.resize(110);
  char carr[16] = { 0 };
	for (int i = 0; i < 11; i++) {
    carr[7] = i;
    for (int j = 0; j < 10; j++) {
      carr[15] = j;
      // keep the last 10 elem for 'Find Fail Test'
      if (i < 10) {
        fwriter.writer << fstring(carr, 16);
      }
      if (i == 0 && j == 0) {
        stat.minKey.assign(carr, carr + 16);
      } else if (i == 9 && j == 9) {
        stat.maxKey.assign(carr, carr + 16);
      }
      keys[i * 10 + j] = string(carr, 16);
    }
	}
  fwriter.close();
  stat.numKeys = 100;
  stat.commonPrefixLen = 0;
  stat.minKeyLen = 16;
  stat.maxKeyLen = 16;
  stat.sumKeyLen = 16 * 100;
}

TerarkIndex* save_reload(TerarkIndex* index, const TerarkIndex::Factory* factory) {
  FileStream writer(index_path, "wb");
  index->SaveMmap([&writer](const void* data, size_t size) {
      writer.ensureWrite(data, size);
    });
  writer.flush();
  writer.close();
  
  return factory->LoadFile(index_path).release();
}

void test_ascend() {
  printf("==== Ascend Test started\n");
  // init data
  clear();
  key_path = "./tmp_key.txt";
  init_data_ascend();
  // build index
  FileStream fp(key_path, "rb");
  NativeDataInput<InputBuffer> tempKeyFileReader(&fp);
  auto factory = rocksdb::TerarkIndex::GetFactory("CompositeIndex");
  rocksdb::TerarkZipTableOptions tableOpt;
  TerarkIndex* index = factory->Build(tempKeyFileReader, tableOpt, stat);
  printf("\tbuild done\n");
  // save & reload
  index_path = "./tmp_index.txt";
  index = save_reload(index, factory);
  printf("\tsave & reload done\n");
  // search test
  for (size_t idx = 0; idx < stat.numKeys; idx++) {
    size_t result = index->Find(keys[idx]);
    assert(idx == result);
  }
  for (size_t idx = stat.numKeys; idx < 110; idx++) {
    size_t result = index->Find(keys[idx]);
    assert(result == size_t(-1));
  }
  printf("\tFind done\n");
  // iterator
  auto iter = index->NewIterator();
  {
    // seek to 1st, next()
    char arr[16] = { 0 };
    assert(iter->SeekToFirst());
    assert(iter->DictRank() == 0);
    assert(fstring(arr, 16) == iter->key());
    for (int i = 0; i < 10; i++) {
      arr[7] = i;
      for (int j = 0; j < 10; j++) {
        arr[15] = j;
        if (i == 0 && j == 0)
          continue;
        assert(iter->Next());
        assert(iter->DictRank() == i * 10 + j);
        assert(fstring(arr, 16) == iter->key());
      }
    }
    assert(iter->Next() == false);
  }
  {
    // seek to last, prev()
    char arr[16] = { 0 };
    assert(iter->SeekToLast());
    assert(iter->DictRank() == 99);
    for (int i = 9; i >= 0; i--) {
      arr[7] = i;
      for (int j = 9; j >= 0; j--) {
        arr[15] = j;
        if (i == 9 && j == 9) {
          assert(fstring(arr, 16) == iter->key());
          continue;
        }
        assert(iter->Prev());
        assert(iter->DictRank() == i * 10 + j);
        assert(fstring(arr, 16) == iter->key());
      }
    }
    assert(iter->Prev() == false);
  }
  {
    // matches
    char arr[16] = { 0 };
    for (int i = 0; i < 9; i++) {
      arr[7] = i;
      for (int j = 9; j >= 0; j--) {
        arr[15] = j;
        int idx = i * 10 + j;
        assert(iter->Seek(keys[idx]));
        assert(iter->DictRank() == idx);
        assert(fstring(arr, 16) == iter->key());
      }
    }
    // larger than larger @apple
    arr[7] = 20; arr[15] = 0;
    assert(iter->Seek(fstring(arr, 16)) == false);
    /*
    // lower_bound
    char larr[17] = { 0 };
    larr[16] = 1;
    for (int i = 0; i < 9; i++) {
      larr[7] = i;
      for (int j = 9; j >= 0; j--) {
        if (i == 9 && j == 9)
          continue;
        larr[15] = j;
        int idx = i * 10 + j + 1;
        assert(iter->Seek(fstring(larr, 17)));
        assert(iter->DictRank() == idx);
        larr[15] += 1;
        assert(fstring(larr, 16) == iter->key());
      }
    }
    */
  }
  printf("\tIterator done\n");

  ::remove(key_path.c_str());
  ::remove(index_path.c_str());
}

int main(int argc, char** argv) {
  printf("EXAGGERATE\n");
  test_ascend();
  return 0;
}
