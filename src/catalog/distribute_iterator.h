/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_CATALOG_DISTRIBUTE_ITERATOR_H_
#define SRC_CATALOG_DISTRIBUTE_ITERATOR_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/hash.h"
#include "base/kv_iterator.h"
#include "client/tablet_client.h"
#include "storage/table.h"
#include "vm/catalog.h"

namespace openmldb {
namespace catalog {

using Tables = std::map<uint32_t, std::shared_ptr<::openmldb::storage::Table>>;

class FullTableIterator : public ::hybridse::codec::ConstIterator<uint64_t, ::hybridse::codec::Row> {
 public:
    FullTableIterator(uint32_t tid, std::shared_ptr<Tables> tables,
            const std::map<uint32_t, std::shared_ptr<::openmldb::client::TabletClient>>& tablet_clients);
    void Seek(const uint64_t& ts) override {
        LOG(ERROR) << "Unsupport Seek in FullTableIterator";
    }

    void SeekToFirst() override;
    bool Valid() const override;
    void Next() override;
    const ::hybridse::codec::Row& GetValue() override;
    bool IsSeekable() const override { return true; }
    // the key maybe the row num
    const uint64_t& GetKey() const override { return key_; }

 private:
    bool NextFromLocal();
    bool NextFromRemote();
    void Reset();
    void EndLocal();

 private:
    uint32_t tid_;
    std::shared_ptr<Tables> tables_;
    std::map<uint32_t, std::shared_ptr<openmldb::client::TabletClient>> tablet_clients_;
    bool in_local_;
    uint32_t cur_pid_;
    std::unique_ptr<::openmldb::storage::TableIterator> it_;
    std::shared_ptr<::openmldb::base::TraverseKvIterator> kv_it_;
    uint64_t key_;
    uint64_t last_ts_;
    std::string last_pk_;
    ::hybridse::codec::Row value_;
    std::vector<std::shared_ptr<::google::protobuf::Message>> response_vec_;
};

class RemoteWindowIterator : public ::hybridse::vm::RowIterator {
 public:
    RemoteWindowIterator(uint32_t tid, uint32_t pid, const std::string& index_name,
            const std::shared_ptr<::openmldb::base::KvIterator>& kv_it,
            const std::shared_ptr<openmldb::client::TabletClient>& client);

    bool Valid() const override;

    void Next() override;

    const uint64_t& GetKey() const override { return ts_; }

    const ::hybridse::codec::Row& GetValue() override;

    // seek to the first element whose key is less or equal to `key`
    // or to the end if not found
    void Seek(const uint64_t& key) override;

    void SeekToFirst() override {
        DLOG(INFO) << "RemoteWindowIterator SeekToFirst";
    }
    bool IsSeekable() const override { return true; }

 private:
    void SetTs();
    void ScanRemote(uint64_t key, uint32_t ts_cnt);

 private:
    uint32_t tid_;
    uint32_t pid_;
    std::string index_name_;
    std::shared_ptr<::openmldb::base::KvIterator> kv_it_;
    std::vector<std::shared_ptr<::google::protobuf::Message>> response_vec_;
    std::shared_ptr<openmldb::client::TabletClient> tablet_client_;
    ::hybridse::codec::Row row_;
    bool is_traverse_data_;
    std::string pk_;
    mutable uint64_t ts_;
    uint32_t ts_cnt_;
};

class DistributeWindowIterator : public ::hybridse::codec::WindowIterator {
 public:
    DistributeWindowIterator(uint32_t tid, uint32_t pid_num, std::shared_ptr<Tables> tables,
            uint32_t index, const std::string& index_name,
            const std::map<uint32_t, std::shared_ptr<::openmldb::client::TabletClient>>& tablet_clients);
    void Seek(const std::string& key) override;
    void SeekToFirst() override;
    void Next() override;
    bool Valid() override;
    std::unique_ptr<::hybridse::codec::RowIterator> GetValue() override;
    ::hybridse::codec::RowIterator* GetRawValue() override;
    const ::hybridse::codec::Row GetKey() override;

 public:
    using IT = std::unique_ptr<::hybridse::codec::WindowIterator>;
    using KV_IT = std::shared_ptr<::openmldb::base::KvIterator>;

    // structure used for iterator result by Seek or SeekToFirst
    struct ItStat {
        // if pid < 0, stat is invalid
        int32_t pid;
        ::hybridse::codec::WindowIterator* it;
        KV_IT kv_it;

        ItStat() = delete;
        ItStat(int32_t p, ::hybridse::codec::WindowIterator* i, KV_IT kv) : pid(p), it(i), kv_it(kv) {}
    };

 private:
    void Reset();

    ItStat SeekByKey(const std::string& key) const;

 private:
    const uint32_t tid_;
    const uint32_t pid_num_;
    std::shared_ptr<Tables> tables_;
    std::map<uint32_t, std::shared_ptr<openmldb::client::TabletClient>> tablet_clients_;
    const uint32_t index_;
    const std::string index_name_;

    uint32_t cur_pid_;
    // iterator to locally data
    IT it_;
    // iterator to remote data, only zero or one of `it_` and `kv_it_` can be non-null
    KV_IT kv_it_;
    // underlaying data pointed by `kv_it_`
    std::vector<std::shared_ptr<::google::protobuf::Message>> response_vec_;
};

}  // namespace catalog
}  // namespace openmldb
#endif  // SRC_CATALOG_DISTRIBUTE_ITERATOR_H_
