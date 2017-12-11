//
// tablet_impl_test.cc
// Copyright (C) 2017 4paradigm.com
// Author wangtaize 
// Date 2017-04-05
//

#include "tablet/tablet_impl.h"
#include "proto/tablet.pb.h"
#include "base/kv_iterator.h"
#include "gtest/gtest.h"
#include "logging.h"
#include "timer.h"
#include "base/schema_codec.h"
#include <gflags/gflags.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <sys/stat.h> 
#include <fcntl.h>

DECLARE_string(db_root_path);

namespace rtidb {
namespace tablet {

uint32_t counter = 10;

inline std::string GenRand() {
    return std::to_string(rand() % 10000000 + 1);
}


class MockClosure : public ::google::protobuf::Closure {

public:
    MockClosure() {}
    ~MockClosure() {}
    void Run() {}

};

class TabletImplTest : public ::testing::Test {

public:
    TabletImplTest() {}
    ~TabletImplTest() {}
};

TEST_F(TabletImplTest, Get) {
    TabletImpl tablet;
    tablet.Init();
    // table not found
    {
        ::rtidb::api::GetRequest request;
        request.set_tid(1);
        request.set_pid(0);
        request.set_key("test");
        request.set_ts(0);
        ::rtidb::api::GetResponse response;
        MockClosure closure;
        tablet.Get(NULL, &request, &response, &closure);
        ASSERT_EQ(-1, response.code());
    }
    // create table
    uint32_t id = counter++;
    {
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_wal(true);
        table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
        ::rtidb::api::CreateTableResponse response;
        MockClosure closure;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());
    }
    // key not found
    {
        ::rtidb::api::GetRequest request;
        request.set_tid(id);
        request.set_pid(1);
        request.set_key("test");
        request.set_ts(0);
        ::rtidb::api::GetResponse response;
        MockClosure closure;
        tablet.Get(NULL, &request, &response, &closure);
        ASSERT_EQ(1, response.code());
    }
    // put some key
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test");
        prequest.set_time(10);
        prequest.set_value("test10");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        MockClosure closure;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test");
        prequest.set_time(9);
        prequest.set_value("test9");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        MockClosure closure;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    // get the 10
    {        
        ::rtidb::api::GetRequest request;
        request.set_tid(id);
        request.set_pid(1);
        request.set_key("test");
        request.set_ts(0);
        ::rtidb::api::GetResponse response;
        MockClosure closure;
        tablet.Get(NULL, &request, &response, &closure);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ("test10", response.value());
    }
    // get the 9 
    {        
        ::rtidb::api::GetRequest request;
        request.set_tid(id);
        request.set_pid(1);
        request.set_key("test");
        request.set_ts(9);
        ::rtidb::api::GetResponse response;
        MockClosure closure;
        tablet.Get(NULL, &request, &response, &closure);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ("test9", response.value());
    }

}

TEST_F(TabletImplTest, CreateTableWithSchema) {
    
    TabletImpl tablet;
    tablet.Init();
    {
        uint32_t id = counter++;
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_wal(true);
        table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
        ::rtidb::api::CreateTableResponse response;
        MockClosure closure;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());
        

        //get schema
        ::rtidb::api::GetTableSchemaRequest request0;
        request0.set_tid(id);
        request0.set_pid(1);
        ::rtidb::api::GetTableSchemaResponse response0;
        tablet.GetTableSchema(NULL, &request0, &response0, &closure);
        ASSERT_EQ("", response0.schema());
        
    }
    {
        std::vector<::rtidb::base::ColumnDesc> columns;
        ::rtidb::base::ColumnDesc desc1;
        desc1.type = ::rtidb::base::ColType::kString;
        desc1.name = "card";
        desc1.add_ts_idx = true;
        columns.push_back(desc1);

        ::rtidb::base::ColumnDesc desc2;
        desc2.type = ::rtidb::base::ColType::kDouble;
        desc2.name = "amt";
        desc2.add_ts_idx = false;
        columns.push_back(desc2);

        ::rtidb::base::ColumnDesc desc3;
        desc3.type = ::rtidb::base::ColType::kInt32;
        desc3.name = "apprv_cde";
        desc3.add_ts_idx = false;
        columns.push_back(desc3);

        ::rtidb::base::SchemaCodec codec;
        std::string buffer;
        codec.Encode(columns, buffer);
        uint32_t id = counter++;
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_wal(true);
        table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
        table_meta->set_schema(buffer);
        ::rtidb::api::CreateTableResponse response;
        MockClosure closure;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());

        ::rtidb::api::GetTableSchemaRequest request0;
        request0.set_tid(id);
        request0.set_pid(1);
        ::rtidb::api::GetTableSchemaResponse response0;
        tablet.GetTableSchema(NULL, &request0, &response0, &closure);
        ASSERT_TRUE(response0.schema().size() != 0);

        std::vector<::rtidb::base::ColumnDesc> ncolumns;
        codec.Decode(response0.schema(), ncolumns);
        ASSERT_EQ(3, ncolumns.size());
        ASSERT_EQ(::rtidb::base::ColType::kString, ncolumns[0].type);
        ASSERT_EQ("card", ncolumns[0].name);
        ASSERT_EQ(::rtidb::base::ColType::kDouble, ncolumns[1].type);
        ASSERT_EQ("amt", ncolumns[1].name);
        ASSERT_EQ(::rtidb::base::ColType::kInt32, ncolumns[2].type);
        ASSERT_EQ("apprv_cde", ncolumns[2].name);
    }
    
}

TEST_F(TabletImplTest, TTL) {
    uint32_t id = counter++;
    uint64_t now = ::baidu::common::timer::get_micros() / 1000;
    TabletImpl tablet;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_wal(true);
    table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
    // 1 minutes
    table_meta->set_ttl(1);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(now);
        prequest.set_value("test1");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());

    }
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(now - 2 * 60 * 1000);
        prequest.set_value("test2");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }

}



TEST_F(TabletImplTest, CreateTable) {
    uint32_t id = counter++;
    TabletImpl tablet;
    tablet.Init();
    {
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_wal(true);
        table_meta->set_ttl(0);
        ::rtidb::api::CreateTableResponse response;
        MockClosure closure;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());
		std::string file = FLAGS_db_root_path + "/" + std::to_string(id) +"_" + std::to_string(1) + "/table_meta.txt";
        int fd = open(file.c_str(), O_RDONLY);
		ASSERT_GT(fd, 0);
		google::protobuf::io::FileInputStream fileInput(fd);
		fileInput.SetCloseOnDelete(true);
		::rtidb::api::TableMeta table_meta_test;
		google::protobuf::TextFormat::Parse(&fileInput, &table_meta_test);
		ASSERT_EQ(table_meta_test.tid(), id);
		ASSERT_STREQ(table_meta_test.name().c_str(), "t0");
		
        table_meta->set_name("");
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(8, response.code());
    }
    {
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_ttl(0);
        ::rtidb::api::CreateTableResponse response;
        MockClosure closure;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(8, response.code());
    }

}

TEST_F(TabletImplTest, Put) {
    TabletImpl tablet;
    uint32_t id = counter++;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(0);
    table_meta->set_wal(true);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());

    ::rtidb::api::PutRequest prequest;
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(2);
    prequest.set_pid(2);
    ::rtidb::api::PutResponse presponse;
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(10, presponse.code());
    prequest.set_tid(id);
    prequest.set_pid(1);
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(0, presponse.code());
}

TEST_F(TabletImplTest, Scan_with_duplicate_skip) {
    TabletImpl tablet;
    uint32_t id = counter++;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(0);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9527);
        prequest.set_value("testx");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }

    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9528);
        prequest.set_value("testx");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9528);
        prequest.set_value("testx");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }

    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9529);
        prequest.set_value("testx");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    ::rtidb::api::ScanRequest sr;
    sr.set_tid(id);
    sr.set_pid(1);
    sr.set_pk("test1");
    sr.set_st(9530);
    sr.set_et(0);
    sr.set_enable_remove_duplicated_record(true);
    ::rtidb::api::ScanResponse srp;
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.code());
    ASSERT_EQ(3, srp.count());
}

TEST_F(TabletImplTest, Scan_with_limit) {
    TabletImpl tablet;
    uint32_t id = counter++;

    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(0);
    table_meta->set_wal(true);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9527);
        prequest.set_value("test0");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }

    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9528);
        prequest.set_value("test0");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }

    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9529);
        prequest.set_value("test0");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    ::rtidb::api::ScanRequest sr;
    sr.set_tid(id);
    sr.set_pid(1);
    sr.set_pk("test1");
    sr.set_st(9530);
    sr.set_et(9526);
    sr.set_limit(2);
    ::rtidb::api::ScanResponse srp;
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.code());
    ASSERT_EQ(2, srp.count());
}

TEST_F(TabletImplTest, Scan) {
    TabletImpl tablet;
    uint32_t id = counter++;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(0);
    table_meta->set_wal(true);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
    ::rtidb::api::ScanRequest sr;
    sr.set_tid(2);
    sr.set_pk("test1");
    sr.set_st(9528);
    sr.set_et(9527);
    sr.set_limit(10);
    ::rtidb::api::ScanResponse srp;
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.pairs().size());
    ASSERT_EQ(10, srp.code());

    sr.set_tid(id);
    sr.set_pid(1);
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.code());
    ASSERT_EQ(0, srp.count());

    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9527);
        prequest.set_value("test0");
        prequest.set_tid(2);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);

        ASSERT_EQ(10, presponse.code());
        prequest.set_tid(id);
        prequest.set_pid(1);
        tablet.Put(NULL, &prequest, &presponse,
                &closure);

        ASSERT_EQ(0, presponse.code());

    }
    {
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9528);
        prequest.set_value("test0");
        prequest.set_tid(2);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);

        ASSERT_EQ(10, presponse.code());
        prequest.set_tid(id);
        prequest.set_pid(1);

        tablet.Put(NULL, &prequest, &presponse,
                &closure);

        ASSERT_EQ(0, presponse.code());

    }
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.code());
    ASSERT_EQ(1, srp.count());

}

TEST_F(TabletImplTest, GC) {
    TabletImpl tablet;
    uint32_t id = counter ++;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(1);
    table_meta->set_wal(true);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());

    ::rtidb::api::PutRequest prequest;
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(id);
    prequest.set_pid(1);
    ::rtidb::api::PutResponse presponse;
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    uint64_t now = ::baidu::common::timer::get_micros() / 1000;
    prequest.set_time(now);
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ::rtidb::api::ScanRequest sr;
    sr.set_tid(id);
    sr.set_pid(1);
    sr.set_pk("test1");
    sr.set_st(now);
    sr.set_et(9527);
    sr.set_limit(10);
    ::rtidb::api::ScanResponse srp;
    tablet.Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(0, srp.code());
    ASSERT_EQ(1, srp.count());
}

TEST_F(TabletImplTest, DropTable) {
    TabletImpl tablet;
    uint32_t id = counter++;
    tablet.Init();
    MockClosure closure;
    ::rtidb::api::DropTableRequest dr;
    dr.set_tid(id);
    dr.set_pid(1);
    ::rtidb::api::DropTableResponse drs;
    tablet.DropTable(NULL, &dr, &drs, &closure);
    ASSERT_EQ(-1, drs.code());

    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(1);
    table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
    ::rtidb::api::CreateTableResponse response;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());

    ::rtidb::api::PutRequest prequest;
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(id);
    prequest.set_pid(1);
    ::rtidb::api::PutResponse presponse;
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(0, presponse.code());
    tablet.DropTable(NULL, &dr, &drs, &closure);
    ASSERT_EQ(0, drs.code());
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
}

TEST_F(TabletImplTest, Recover) {
    uint32_t id = counter++;
    MockClosure closure;
    {
        TabletImpl tablet;
        tablet.Init();
        ::rtidb::api::CreateTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_ttl(0);
        table_meta->set_seg_cnt(128);
        table_meta->set_term(1024);
        table_meta->add_replicas("127.0.0.1:9527");
        table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
        ::rtidb::api::CreateTableResponse response;
        tablet.CreateTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9527);
        prequest.set_value("test0");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
    }
    // recover
    {
        TabletImpl tablet;
        tablet.Init();
        ::rtidb::api::LoadTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_seg_cnt(64);
        table_meta->add_replicas("127.0.0.1:9530");
        table_meta->add_replicas("127.0.0.1:9531");
        ::rtidb::api::GeneralResponse response;
        tablet.LoadTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());

		std::string file = FLAGS_db_root_path + "/" + std::to_string(id) +"_" + std::to_string(1) + "/table_meta.txt";
        int fd = open(file.c_str(), O_RDONLY);
		ASSERT_GT(fd, 0);
		google::protobuf::io::FileInputStream fileInput(fd);
		fileInput.SetCloseOnDelete(true);
		::rtidb::api::TableMeta table_meta_test;
		google::protobuf::TextFormat::Parse(&fileInput, &table_meta_test);
		ASSERT_EQ(table_meta_test.seg_cnt(), 64);
		ASSERT_EQ(table_meta_test.term(), 1024);
		ASSERT_EQ(table_meta_test.replicas_size(), 2);
		ASSERT_STREQ(table_meta_test.replicas(0).c_str(), "127.0.0.1:9530");

        ::rtidb::api::ScanRequest sr;
        sr.set_tid(id);
        sr.set_pid(1);
        sr.set_pk("test1");
        sr.set_st(9530);
        sr.set_et(9526);
        ::rtidb::api::ScanResponse srp;
        tablet.Scan(NULL, &sr, &srp, &closure);
        ASSERT_EQ(0, srp.code());
        ASSERT_EQ(1, srp.count());
        ::rtidb::api::GeneralRequest grq;
        grq.set_tid(id);
        grq.set_pid(1);
        ::rtidb::api::GeneralResponse grp;
        grp.set_code(-1);
        tablet.MakeSnapshot(NULL, &grq, &grp, &closure);
        ASSERT_EQ(0, grp.code());
        ::rtidb::api::PutRequest prequest;
        prequest.set_pk("test1");
        prequest.set_time(9528);
        prequest.set_value("test1");
        prequest.set_tid(id);
        prequest.set_pid(1);
        ::rtidb::api::PutResponse presponse;
        tablet.Put(NULL, &prequest, &presponse,
                &closure);
        ASSERT_EQ(0, presponse.code());
        sleep(2);
    }

    {
        TabletImpl tablet;
        tablet.Init();
        ::rtidb::api::LoadTableRequest request;
        ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
        table_meta->set_name("t0");
        table_meta->set_tid(id);
        table_meta->set_pid(1);
        table_meta->set_ttl(0);
        table_meta->set_mode(::rtidb::api::TableMode::kTableLeader);
        ::rtidb::api::GeneralResponse response;
        tablet.LoadTable(NULL, &request, &response,
                &closure);
        ASSERT_EQ(0, response.code());
        ::rtidb::api::ScanRequest sr;
        sr.set_tid(id);
        sr.set_pid(1);
        sr.set_pk("test1");
        sr.set_st(9530);
        sr.set_et(9526);
        ::rtidb::api::ScanResponse srp;
        tablet.Scan(NULL, &sr, &srp, &closure);
        ASSERT_EQ(0, srp.code());
        ASSERT_EQ(2, srp.count());
    }

}

TEST_F(TabletImplTest, DropTableFollower) {
    uint32_t id = counter++;
    TabletImpl tablet;
    tablet.Init();
    MockClosure closure;
    ::rtidb::api::DropTableRequest dr;
    dr.set_tid(id);
    dr.set_pid(1);
    ::rtidb::api::DropTableResponse drs;
    tablet.DropTable(NULL, &dr, &drs, &closure);
    ASSERT_EQ(-1, drs.code());

    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(1);
    table_meta->set_mode(::rtidb::api::TableMode::kTableFollower);
    table_meta->add_replicas("127.0.0.1:9527");
    ::rtidb::api::CreateTableResponse response;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());
    ::rtidb::api::PutRequest prequest;
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(id);
    prequest.set_pid(1);
    ::rtidb::api::PutResponse presponse;
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    //ReadOnly
    ASSERT_EQ(20, presponse.code());

    tablet.DropTable(NULL, &dr, &drs, &closure);
    ASSERT_EQ(0, drs.code());
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(id);
    prequest.set_pid(1);
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(10, presponse.code());
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());

}

TEST_F(TabletImplTest, Snapshot) {
    TabletImpl tablet;
    uint32_t id = counter++;
    tablet.Init();
    ::rtidb::api::CreateTableRequest request;
    ::rtidb::api::TableMeta* table_meta = request.mutable_table_meta();
    table_meta->set_name("t0");
    table_meta->set_tid(id);
    table_meta->set_pid(1);
    table_meta->set_ttl(0);
    table_meta->set_wal(true);
    ::rtidb::api::CreateTableResponse response;
    MockClosure closure;
    tablet.CreateTable(NULL, &request, &response,
            &closure);
    ASSERT_EQ(0, response.code());

    ::rtidb::api::PutRequest prequest;
    prequest.set_pk("test1");
    prequest.set_time(9527);
    prequest.set_value("test0");
    prequest.set_tid(id);
    prequest.set_pid(2);
    ::rtidb::api::PutResponse presponse;
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(10, presponse.code());
    prequest.set_tid(id);
    prequest.set_pid(1);
    tablet.Put(NULL, &prequest, &presponse,
            &closure);
    ASSERT_EQ(0, presponse.code());

    ::rtidb::api::GeneralRequest grequest;
    ::rtidb::api::GeneralResponse gresponse;
    grequest.set_tid(id);
    grequest.set_pid(1);
    tablet.PauseSnapshot(NULL, &grequest, &gresponse,
            &closure);
    ASSERT_EQ(0, gresponse.code());

    tablet.MakeSnapshot(NULL, &grequest, &gresponse,
            &closure);
    ASSERT_EQ(-1, gresponse.code());

    tablet.RecoverSnapshot(NULL, &grequest, &gresponse,
            &closure);
    ASSERT_EQ(0, gresponse.code());

    tablet.MakeSnapshot(NULL, &grequest, &gresponse,
            &closure);
    ASSERT_EQ(0, gresponse.code());
}



}
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    srand (time(NULL));
    ::baidu::common::SetLogLevel(::baidu::common::INFO);
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_db_root_path = "/tmp/" + ::rtidb::tablet::GenRand();
    return RUN_ALL_TESTS();
}



