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

#include "cmd/sql_cmd.h"

#include <unistd.h>

#include <filesystem>
#include <limits>
#include <memory>
#include <string>

#include "absl/cleanup/cleanup.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "gflags/gflags.h"
#include "gtest/gtest.h"
#include "sdk/mini_cluster.h"
#include "sdk/sql_router.h"
#include "test/util.h"
#include "vm/catalog.h"

DECLARE_bool(interactive);
DEFINE_string(cmd, "", "Set cmd");
DECLARE_string(host);
DECLARE_int32(port);
DECLARE_uint32(traverse_cnt_limit);
DECLARE_string(ssd_root_path);
DECLARE_string(hdd_root_path);
DECLARE_string(recycle_bin_ssd_root_path);
DECLARE_string(recycle_bin_hdd_root_path);

::openmldb::sdk::StandaloneEnv env;

namespace openmldb {
namespace cmd {

::openmldb::sdk::MiniCluster* mc_;

using test::GenRand;
using test::ProcessSQLs;
using test::ExpectResultSetStrEq;

struct CLI {
    ::openmldb::sdk::DBSDK* cs = nullptr;
    ::openmldb::sdk::SQLClusterRouter* sr = nullptr;
};

CLI standalone_cli;
CLI cluster_cli;

class SqlCmdTest : public ::testing::Test {
 public:
    SqlCmdTest() {}
    ~SqlCmdTest() {}
};

class DBSDKTest : public ::testing::TestWithParam<CLI*> {};

TEST_F(SqlCmdTest, showDeployment) {
    auto cli = cluster_cli;
    auto sr = cli.sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("show deployment aa", &status);
    ASSERT_FALSE(status.IsOK());
    ASSERT_EQ(status.msg, "Please enter database first");
}

TEST_F(SqlCmdTest, SelectIntoOutfile) {
    sdk::SQLRouterOptions sql_opt;
    sql_opt.zk_cluster = mc_->GetZkCluster();
    sql_opt.zk_path = mc_->GetZkPath();
    auto router = NewClusterSQLRouter(sql_opt);
    ASSERT_TRUE(router != nullptr);
    std::string name = "test" + GenRand();
    std::string db = "db" + GenRand();
    std::string file_path = "/tmp/data" + GenRand() + ".csv";
    ::hybridse::sdk::Status status;
    bool ok = router->CreateDB(db, &status);
    ASSERT_TRUE(ok);
    router->ExecuteSQL("use " + db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << "error msg: " + status.msg;
    router->ExecuteSQL("SET @@execute_mode='online';", &status);
    ASSERT_TRUE(status.IsOK()) << "error msg: " + status.msg;
    std::string ddl = "create table " + name +
                      "("
                      "col1 string, col2 int);";
    ok = router->ExecuteDDL(db, ddl, &status);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(router->RefreshCatalog());

    std::string insert = "insert into " + name + " (col1) " + " values('key1');";
    ASSERT_TRUE(router->ExecuteInsert(db, insert, &status));
    ASSERT_TRUE(router->RefreshCatalog());

    // True
    std::string select_into_sql = "select * from " + name + " into outfile '" + file_path + "'";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_TRUE(status.IsOK()) << "error msg: " + status.msg;
    // Check file
    std::ifstream file;
    file.open(file_path);
    file.seekg(0, file.end);
    int length = file.tellg();
    file.seekg(0, file.beg);
    char* data = new char[length + 1];
    data[length] = '\0';
    file.read(data, length);
    ASSERT_EQ(strcmp(data, "col1,col2\nkey1,null"), 0);
    delete[] data;
    file.close();

    // True
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (mode = 'overwrite')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_TRUE(status.IsOK());

    // True
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (mode = 'append')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_TRUE(status.IsOK());

    file.open(file_path);
    file.seekg(0, file.end);
    int append_length = file.tellg();
    file.seekg(0, file.beg);
    char* append_data = new char[append_length + 1];
    append_data[append_length] = '\0';
    file.read(append_data, append_length);
    ASSERT_EQ(strcmp(append_data, "col1,col2\nkey1,null\ncol1,col2\nkey1,null"), 0);
    delete[] append_data;
    file.close();

    // Fail - File exists
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (mode = 'error_if_exists')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // Fail - Mode un-supported
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (mode = 'error')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Format un-supported
    select_into_sql =
        "select * from " + name + " into outfile '" + file_path + "' options (mode = 'overwrite', format = 'parquet')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - File path error
    select_into_sql = "select * from " + name + " into outfile 'file:////tmp/data.csv'";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Option un-supported
    select_into_sql =
        "select * from " + name + " into outfile '" + file_path + "' options (mode = 'overwrite', test = 'null')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Type un-supproted
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (mode = 1)";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Type un-supproted
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (quote = '__')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Type un-supproted
    select_into_sql = "select * from " + name + " into outfile '" + file_path + "' options (delimiter = '')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    // False - Delimiter can't include quote
    select_into_sql =
        "select * from " + name + " into outfile '" + file_path + "' options (quote = '_', delimiter = '__')";
    router->ExecuteSQL(select_into_sql, &status);
    ASSERT_FALSE(status.IsOK());

    router->ExecuteSQL("drop table " + name, &status);
    router->DropDB(db, &status);
    ASSERT_TRUE(status.IsOK());
    remove(file_path.c_str());
}

TEST_P(DBSDKTest, CreateDatabase) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    hybridse::sdk::Status status;

    auto db1 = absl::StrCat("db_", GenRand());

    ProcessSQLs(sr, {
                        absl::StrCat("CREATE DATABASE ", db1),
                        absl::StrCat("CREATE DATABASE IF NOT EXISTS ", db1),
                    });

    sr->ExecuteSQL(absl::StrCat("CREATE DATABASE ", db1), &status);
    EXPECT_FALSE(status.IsOK());

    ProcessSQLs(sr, {absl::StrCat("DROP DATABASE ", db1)});
}

TEST_P(DBSDKTest, Select) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    hybridse::sdk::Status status;
    if (cs->IsClusterMode()) {
        sr->ExecuteSQL("SET @@execute_mode='online';", &status);
        ASSERT_TRUE(status.IsOK()) << "error msg: " + status.msg;
    }
    std::string db = "db" + GenRand();
    sr->ExecuteSQL("create database " + db + ";", &status);
    ASSERT_TRUE(status.IsOK());
    sr->ExecuteSQL("use " + db + ";", &status);
    ASSERT_TRUE(status.IsOK());
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c3, ts=c7, abs_ttl=0, ttl_type=absolute));";
    sr->ExecuteSQL(create_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string insert_sql = "insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");";
    sr->ExecuteSQL(insert_sql, &status);
    ASSERT_TRUE(status.IsOK());
    auto rs = sr->ExecuteSQL("select * from trans", &status);
    ASSERT_TRUE(status.IsOK());
    ASSERT_EQ(1, rs->Size());
    sr->ExecuteSQL("drop table trans;", &status);
    ASSERT_TRUE(status.IsOK());
    sr->ExecuteSQL("drop database " + db + ";", &status);
    ASSERT_TRUE(status.IsOK());
}

TEST_F(SqlCmdTest, SelectMultiPartition) {
    auto sr = cluster_cli.sr;
    std::string db_name = "test" + GenRand();
    std::string name = "table" + GenRand();
    std::string ddl = "create table " + name +
                      "("
                      "col1 int not null,"
                      "col2 bigint default 112 not null,"
                      "col4 string default 'test4' not null,"
                      "col5 date default '2000-01-01' not null,"
                      "col6 timestamp default 10000 not null,"
                      "index(key=col1, ts=col2)) options(partitionnum=8);";
    ProcessSQLs(sr, {"set @@execute_mode = 'online'",
            absl::StrCat("create database ", db_name, ";"),
            absl::StrCat("use ", db_name, ";"),
            ddl});
    std::string sql;
    int expect = 1000;
    hybridse::sdk::Status status;
    for (int i = 0; i < expect; i++) {
        sql = "insert into " + name + " values(" + std::to_string(i) + ", 1, '1', '2021-01-01', 1);";
        ASSERT_TRUE(sr->ExecuteInsert(db_name, sql, &status));
    }
    auto res = sr->ExecuteSQL(db_name, "select * from " + name, &status);
    ASSERT_TRUE(res);
    int count = 0;
    while (res->Next()) {
        count++;
    }
    EXPECT_EQ(count, expect);
    ProcessSQLs(sr, {absl::StrCat("drop table ", name, ";"), absl::StrCat("drop database ", db_name, ";")});
}

TEST_P(DBSDKTest, Desc) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    hybridse::sdk::Status status;
    if (cs->IsClusterMode()) {
        sr->ExecuteSQL("SET @@execute_mode='online';", &status);
        ASSERT_TRUE(status.IsOK()) << "error msg: " + status.msg;
    }
    std::string db = "db" + GenRand();
    sr->ExecuteSQL("create database " + db + ";", &status);
    ASSERT_TRUE(status.IsOK());
    sr->ExecuteSQL("use " + db + ";", &status);
    ASSERT_TRUE(status.IsOK());
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date) options(storage_mode='Memory');";
    sr->ExecuteSQL(create_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    std::string desc_sql = "desc trans;";
    auto rs = sr->ExecuteSQL(desc_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    ASSERT_EQ(3, rs->Size());

    std::string expect_schema =
        " --- ------- ----------- ------ --------- \n"
        "  #   Field   Type        Null   Default  \n"
        " --- ------- ----------- ------ --------- \n"
        "  1   c1      Varchar     YES             \n"
        "  2   c3      Int         YES             \n"
        "  3   c4      BigInt      YES             \n"
        "  4   c5      Float       YES             \n"
        "  5   c6      Double      YES             \n"
        "  6   c7      Timestamp   YES             \n"
        "  7   c8      Date        YES             \n"
        " --- ------- ----------- ------ --------- \n";

    std::string expect_options =
        " -------------- \n"
        "  storage_mode  \n"
        " -------------- \n"
        "  Memory        \n"
        " -------------- \n\n";

    // index name is dynamically assigned. do not check here
    std::vector<std::string> expect = {expect_schema, "", expect_options};
    int count = 0;
    while (rs->Next()) {
        std::string val;
        rs->GetString(0, &val);
        if (!expect[count].empty()) {
            EXPECT_EQ(expect[count], val);
        }
        count++;
    }
    sr->ExecuteSQL("drop table trans;", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL("drop database " + db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
}

TEST_F(SqlCmdTest, LoadData) {
    sr = standalone_cli.sr;
    cs = standalone_cli.cs;
    HandleSQL("create database test1;");
    HandleSQL("use test1;");
    std::string create_sql = "create table trans (c1 string, c2 int);";
    HandleSQL(create_sql);
    std::string file_name = "./myfile.csv";
    std::ofstream ofile;
    ofile.open(file_name);
    ofile << "c1,c2" << std::endl;
    for (int i = 0; i < 10; i++) {
        ofile << "aa" << i << "," << i << std::endl;
    }
    ofile.close();
    std::string load_sql = "LOAD DATA INFILE '" + file_name + "' INTO TABLE trans;";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(load_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    auto result = sr->ExecuteSQL("select * from trans;", &status);
    ASSERT_TRUE(status.IsOK());
    ASSERT_EQ(10, result->Size());
    HandleSQL("drop table trans;");
    HandleSQL("drop database test1;");
    unlink(file_name.c_str());
}

TEST_P(DBSDKTest, Deploy) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test1;");
    HandleSQL("use test1;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c3, ts=c7, abs_ttl=0, ttl_type=absolute));";
    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }

    std::string deploy_sql =
        "deploy demo SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM trans "
        " WINDOW w1 AS (PARTITION BY trans.c1 ORDER BY trans.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);";

    hybridse::sdk::Status status;
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string msg;
    ASSERT_FALSE(cs->GetNsClient()->DropTable("test1", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropProcedure("test1", "demo", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test1", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test1", msg));

    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_FALSE(status.IsOK());
}

TEST_P(DBSDKTest, DeployWithSameIndex) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test1;");
    HandleSQL("use test1;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c1, ts=c7, ttl=1, ttl_type=latest));";

    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }

    // origin index
    std::string msg;
    auto ns_client = cs->GetNsClient();
    std::vector<::openmldb::nameserver::TableInfo> tables;
    ASSERT_TRUE(ns_client->ShowTable("trans", "test1", false, tables, msg));
    ::openmldb::nameserver::TableInfo table = tables[0];

    ASSERT_EQ(table.column_key_size(), 1);
    ::openmldb::common::ColumnKey column_key = table.column_key(0);
    ASSERT_EQ(column_key.col_name_size(), 1);
    ASSERT_EQ(column_key.col_name(0), "c1");
    ASSERT_EQ(column_key.ts_name(), "c7");
    ASSERT_TRUE(column_key.has_ttl());
    ASSERT_EQ(column_key.ttl().ttl_type(), ::openmldb::type::TTLType::kLatestTime);
    ASSERT_EQ(column_key.ttl().lat_ttl(), 1);

    std::string deploy_sql =
        "deploy demo SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM trans "
        " WINDOW w1 AS (PARTITION BY trans.c1 ORDER BY trans.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_TRUE(status.IsOK());

    // new index, update ttl
    tables.clear();
    ASSERT_TRUE(ns_client->ShowTable("trans", "test1", false, tables, msg));
    table = tables[0];

    ASSERT_EQ(table.column_key_size(), 1);
    column_key = table.column_key(0);
    ASSERT_EQ(column_key.col_name_size(), 1);
    ASSERT_EQ(column_key.col_name(0), "c1");
    ASSERT_EQ(column_key.ts_name(), "c7");
    ASSERT_TRUE(column_key.has_ttl());
    ASSERT_EQ(column_key.ttl().ttl_type(), ::openmldb::type::TTLType::kLatestTime);
    ASSERT_EQ(column_key.ttl().lat_ttl(), 2);

    // type mismatch case
    create_sql =
        "create table trans1 (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c1, ts=c7, ttl=1m, ttl_type=absolute));";
    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans1 values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }
    deploy_sql =
        "deploy demo SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM trans1 "
        " WINDOW w1 AS (PARTITION BY trans1.c1 ORDER BY trans1.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_FALSE(status.IsOK());
    ASSERT_EQ(status.msg, "new ttl type kLatestTime doesn't match the old ttl type kAbsoluteTime");


    ASSERT_FALSE(cs->GetNsClient()->DropTable("test1", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropProcedure("test1", "demo", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test1", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test1", "trans1", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test1", msg));
}

TEST_P(DBSDKTest, DeployCol) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test2;");
    HandleSQL("use test2;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c1, ts=c4, abs_ttl=0, ttl_type=absolute));";
    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }

    std::string deploy_sql =
        "deploy demo SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM trans "
        " WINDOW w1 AS (PARTITION BY trans.c1 ORDER BY trans.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string msg;
    ASSERT_FALSE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropProcedure("test2", "demo", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test2", msg));
}

TEST_P(DBSDKTest, DeployOptions) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test2;");
    HandleSQL("use test2;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c1, ts=c4, abs_ttl=0, ttl_type=absolute));";
    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }

    std::string deploy_sql =
        "deploy demo OPTIONS(long_windows='w1:100') SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM trans "
        " WINDOW w1 AS (PARTITION BY trans.c1 ORDER BY trans.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string msg;
    ASSERT_FALSE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropProcedure("test2", "demo", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test2", msg));
}

TEST_P(DBSDKTest, DeployLongWindows) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test2;");
    HandleSQL("use test2;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(key=c1, ts=c4, ttl=0, ttl_type=latest));";
    HandleSQL(create_sql);
    if (!cs->IsClusterMode()) {
        HandleSQL("insert into trans values ('aaa', 11, 22, 1.2, 1.3, 1635247427000, \"2021-05-20\");");
    }

    std::string deploy_sql =
        "deploy demo1 OPTIONS(long_windows='w1:100,w2') SELECT c1, sum(c4) OVER w1 as w1_c4_sum,"
        " max(c5) over w2 as w2_max_c5 FROM trans"
        " WINDOW w1 AS (PARTITION BY trans.c1 ORDER BY trans.c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW),"
        " w2 AS (PARTITION BY trans.c1 ORDER BY trans.c4 ROWS BETWEEN 3 PRECEDING AND CURRENT ROW);";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(deploy_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string msg;
    ASSERT_FALSE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropProcedure("test2", "demo1", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test2", msg));
}

void CreateDBTableForLongWindow(const std::string& base_db, const std::string& base_table) {
    ::hybridse::sdk::Status status;
    bool ok = sr->CreateDB(base_db, &status);
    ASSERT_TRUE(ok) << status.msg;
    std::string ddl =
        "create table " + base_table +
        "(col1 string, col2 string, col3 timestamp, i64_col bigint, i16_col smallint, i32_col int, f_col "
        "float, d_col double, t_col timestamp, s_col string, date_col date, index(key=(col1,col2), ts=col3, "
        "abs_ttl=0, ttl_type=absolute)) "
        "options(partitionnum=8);";
    ok = sr->ExecuteDDL(base_db, ddl, &status);
    ASSERT_TRUE(ok) << status.msg;
    ASSERT_TRUE(sr->RefreshCatalog());

    auto ns_client = cs->GetNsClient();
    std::vector<::openmldb::nameserver::TableInfo> tables;
    std::string msg;
    ASSERT_TRUE(ns_client->ShowTable(base_table, base_db, false, tables, msg));
    ASSERT_EQ(tables.size(), 1) << msg;
}

void PrepareDataForLongWindow(const std::string& base_db, const std::string& base_table) {
    ::hybridse::sdk::Status status;
    for (int i = 1; i <= 11; i++) {
        std::string val = std::to_string(i);
        std::string date;
        if (i < 10) {
            date = absl::StrCat("1900-01-0", std::to_string(i));
        } else {
            date = absl::StrCat("1900-01-", std::to_string(i));
        }
        std::string insert =
            absl::StrCat("insert into ", base_table, " values('str1', 'str2', ", val, ", ", val, ", ", val, ", ", val,
                         ", ", val, ", ", val, ", ", val, ", '", val, "', '", date, "');");
        bool ok = sr->ExecuteInsert(base_db, insert, &status);
        ASSERT_TRUE(ok) << status.msg;
    }
}

void PrepareRequestRowForLongWindow(const std::string& base_db, const std::string& sp_name,
                                    std::shared_ptr<sdk::SQLRequestRow>& req) {  // NOLINT
    ::hybridse::sdk::Status status;
    req = sr->GetRequestRowByProcedure(base_db, sp_name, &status);
    ASSERT_TRUE(status.IsOK());
    ASSERT_TRUE(req->Init(strlen("str1") + strlen("str2") + strlen("11")));
    ASSERT_TRUE(req->AppendString("str1"));
    ASSERT_TRUE(req->AppendString("str2"));
    ASSERT_TRUE(req->AppendTimestamp(11));
    ASSERT_TRUE(req->AppendInt64(11));
    ASSERT_TRUE(req->AppendInt16(11));
    ASSERT_TRUE(req->AppendInt32(11));
    ASSERT_TRUE(req->AppendFloat(11));
    ASSERT_TRUE(req->AppendDouble(11));
    ASSERT_TRUE(req->AppendTimestamp(11));
    ASSERT_TRUE(req->AppendString("11"));
    ASSERT_TRUE(req->AppendDate(11));
    ASSERT_TRUE(req->Build());
}

TEST_P(DBSDKTest, DeployLongWindowsEmpty) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(LONG_WINDOWS='w1:2') select col1, col2,"
        " sum(i64_col) over w1 as w1_sum_i64_col,"
        " sum(i16_col) over w1 as w1_sum_i16_col,"
        " sum(i32_col) over w1 as w1_sum_i32_col,"
        " sum(f_col) over w1 as w1_sum_f_col,"
        " sum(d_col) over w1 as w1_sum_d_col,"
        " sum(t_col) over w1 as w1_sum_t_col,"
        " sum(col3) over w2 as w2_sum_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY " + base_table + ".col1," + base_table + ".col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_sum_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_t_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(0, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        int64_t exp = 11;
        ASSERT_EQ(exp, res->GetInt64Unsafe(2));
        ASSERT_EQ(exp, res->GetInt16Unsafe(3));
        ASSERT_EQ(exp, res->GetInt32Unsafe(4));
        ASSERT_EQ(exp, res->GetFloatUnsafe(5));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(6));
        ASSERT_EQ(exp, res->GetTimeUnsafe(7));
        ASSERT_EQ(exp, res->GetInt64Unsafe(8));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_sum_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_t_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, DeployLongWindowsExecuteSum) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(LONG_WINDOWS='w1:2') select col1, col2,"
        " sum(i64_col) over w1 as w1_sum_i64_col,"
        " sum(i16_col) over w1 as w1_sum_i16_col,"
        " sum(i32_col) over w1 as w1_sum_i32_col,"
        " sum(f_col) over w1 as w1_sum_f_col,"
        " sum(d_col) over w1 as w1_sum_d_col,"
        " sum(t_col) over w1 as w1_sum_t_col,"
        " sum(col3) over w2 as w2_sum_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY " + base_table + ".col1," + base_table + ".col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    PrepareDataForLongWindow(base_db, base_table);
    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_sum_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    for (int i = 5; i >= 1; i--) {
        ASSERT_TRUE(rs->Next());
        ASSERT_EQ("str1|str2", rs->GetStringUnsafe(0));
        ASSERT_EQ(i * 2 - 1, rs->GetInt64Unsafe(1));
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(2));
        ASSERT_EQ(2, rs->GetInt32Unsafe(3));
        std::string aggr_val_str = rs->GetStringUnsafe(4);
        int64_t aggr_val = *reinterpret_cast<int64_t*>(&aggr_val_str[0]);
        ASSERT_EQ(i * 4 - 1, aggr_val);
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(5));
    }

    result_sql = "select * from pre_test_aggr_w1_sum_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_sum_t_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        int64_t exp = 11 + 11 + 19 + 15 + 6;
        ASSERT_EQ(exp, res->GetInt64Unsafe(2));
        ASSERT_EQ(exp, res->GetInt16Unsafe(3));
        ASSERT_EQ(exp, res->GetInt32Unsafe(4));
        ASSERT_EQ(exp, res->GetFloatUnsafe(5));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(6));
        ASSERT_EQ(exp, res->GetTimeUnsafe(7));
        ASSERT_EQ(exp, res->GetInt64Unsafe(8));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_sum_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_sum_t_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, DeployLongWindowsExecuteAvg) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(long_windows='w1:2') select col1, col2,"
        " avg(i64_col) over w1 as w1_avg_i64_col,"
        " avg(i16_col) over w1 as w1_avg_i16_col,"
        " avg(i32_col) over w1 as w1_avg_i32_col,"
        " avg(f_col) over w1 as w1_avg_f_col,"
        " avg(d_col) over w1 as w1_avg_d_col,"
        " avg(i64_col) over w2 as w2_avg_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY col1,col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    PrepareDataForLongWindow(base_db, base_table);
    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_avg_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    for (int i = 5; i >= 1; i--) {
        ASSERT_TRUE(rs->Next());
        ASSERT_EQ("str1|str2", rs->GetStringUnsafe(0));
        ASSERT_EQ(i * 2 - 1, rs->GetInt64Unsafe(1));
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(2));
        ASSERT_EQ(2, rs->GetInt32Unsafe(3));
        std::string aggr_val_str = rs->GetStringUnsafe(4);
        ASSERT_EQ(16, aggr_val_str.size());
        double aggr_sum = *reinterpret_cast<double*>(&aggr_val_str[0]);
        ASSERT_EQ(i * 4 - 1, aggr_sum);
        int64_t aggr_count = *reinterpret_cast<int64_t*>(&aggr_val_str[sizeof(int64_t)]);
        ASSERT_EQ(2, aggr_count);
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(5));
    }

    result_sql = "select * from pre_test_aggr_w1_avg_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_avg_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_avg_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_avg_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        double exp = static_cast<double>(11 + 11 + 19 + 15 + 6) / 7;
        ASSERT_EQ(exp, res->GetDoubleUnsafe(2));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(3));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(4));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(5));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(6));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(7));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_avg_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_avg_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_avg_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_avg_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_avg_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, DeployLongWindowsExecuteMin) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(long_windows='w1:2') select col1, col2,"
        " min(i64_col) over w1 as w1_min_i64_col,"
        " min(i16_col) over w1 as w1_min_i16_col,"
        " min(i32_col) over w1 as w1_min_i32_col,"
        " min(f_col) over w1 as w1_min_f_col,"
        " min(d_col) over w1 as w1_min_d_col,"
        " min(t_col) over w1 as w1_min_t_col,"
        " min(s_col) over w1 as w1_min_s_col,"
        " min(date_col) over w1 as w1_min_date_col,"
        " min(col3) over w2 as w2_min_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY col1,col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    PrepareDataForLongWindow(base_db, base_table);
    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_min_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    for (int i = 5; i >= 1; i--) {
        ASSERT_TRUE(rs->Next());
        ASSERT_EQ("str1|str2", rs->GetStringUnsafe(0));
        ASSERT_EQ(i * 2 - 1, rs->GetInt64Unsafe(1));
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(2));
        ASSERT_EQ(2, rs->GetInt32Unsafe(3));
        std::string aggr_val_str = rs->GetStringUnsafe(4);
        int64_t aggr_val = *reinterpret_cast<int64_t*>(&aggr_val_str[0]);
        ASSERT_EQ(i * 2 - 1, aggr_val);
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(5));
    }

    result_sql = "select * from pre_test_aggr_w1_min_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_t_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_s_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_min_date_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        int64_t exp = 6;
        ASSERT_EQ(exp, res->GetInt64Unsafe(2));
        ASSERT_EQ(exp, res->GetInt16Unsafe(3));
        ASSERT_EQ(exp, res->GetInt32Unsafe(4));
        ASSERT_EQ(exp, res->GetFloatUnsafe(5));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(6));
        ASSERT_EQ(exp, res->GetTimeUnsafe(7));
        ASSERT_EQ("10", res->GetStringUnsafe(8));
        ASSERT_EQ(exp, res->GetDateUnsafe(9));
        ASSERT_EQ(exp, res->GetInt64Unsafe(10));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_min_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_t_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_s_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_min_date_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, DeployLongWindowsExecuteMax) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(long_windows='w1:2') select col1, col2,"
        " max(i64_col) over w1 as w1_max_i64_col,"
        " max(i16_col) over w1 as w1_max_i16_col,"
        " max(i32_col) over w1 as w1_max_i32_col,"
        " max(f_col) over w1 as w1_max_f_col,"
        " max(d_col) over w1 as w1_max_d_col,"
        " max(t_col) over w1 as w1_max_t_col,"
        " max(s_col) over w1 as w1_max_s_col,"
        " max(date_col) over w1 as w1_max_date_col,"
        " max(col3) over w2 as w2_max_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY col1,col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    PrepareDataForLongWindow(base_db, base_table);
    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_max_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    for (int i = 5; i >= 1; i--) {
        ASSERT_TRUE(rs->Next());
        ASSERT_EQ("str1|str2", rs->GetStringUnsafe(0));
        ASSERT_EQ(i * 2 - 1, rs->GetInt64Unsafe(1));
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(2));
        ASSERT_EQ(2, rs->GetInt32Unsafe(3));
        std::string aggr_val_str = rs->GetStringUnsafe(4);
        int64_t aggr_val = *reinterpret_cast<int64_t*>(&aggr_val_str[0]);
        ASSERT_EQ(i * 2, aggr_val);
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(5));
    }

    result_sql = "select * from pre_test_aggr_w1_max_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_t_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_s_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_max_date_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        int64_t exp = 11;
        ASSERT_EQ(exp, res->GetInt64Unsafe(2));
        ASSERT_EQ(exp, res->GetInt16Unsafe(3));
        ASSERT_EQ(exp, res->GetInt32Unsafe(4));
        ASSERT_EQ(exp, res->GetFloatUnsafe(5));
        ASSERT_EQ(exp, res->GetDoubleUnsafe(6));
        ASSERT_EQ(exp, res->GetTimeUnsafe(7));
        ASSERT_EQ("9", res->GetStringUnsafe(8));
        ASSERT_EQ(exp, res->GetDateUnsafe(9));
        ASSERT_EQ(exp, res->GetInt64Unsafe(10));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_max_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_t_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_s_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_max_date_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, DeployLongWindowsExecuteCount) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ::hybridse::sdk::Status status;
    sr->ExecuteSQL("SET @@execute_mode='online';", &status);
    std::string base_table = "t_lw" + GenRand();
    std::string base_db = "d_lw" + GenRand();
    bool ok;
    std::string msg;
    CreateDBTableForLongWindow(base_db, base_table);

    std::string deploy_sql = "deploy test_aggr options(long_windows='w1:2') select col1, col2,"
        " count(*) over w1 as w1_count_all,"
        " count(i64_col) over w1 as w1_count_i64_col,"
        " count(i16_col) over w1 as w1_count_i16_col,"
        " count(i32_col) over w1 as w1_count_i32_col,"
        " count(f_col) over w1 as w1_count_f_col,"
        " count(d_col) over w1 as w1_count_d_col,"
        " count(t_col) over w1 as w1_count_t_col,"
        " count(s_col) over w1 as w1_count_s_col,"
        " count(date_col) over w1 as w1_count_date_col,"
        " count(col3) over w2 as w2_count_col3"
        " from " + base_table +
        " WINDOW w1 AS (PARTITION BY col1,col2 ORDER BY col3"
        " ROWS_RANGE BETWEEN 5 PRECEDING AND CURRENT ROW), "
        " w2 AS (PARTITION BY col1,col2 ORDER BY i64_col"
        " ROWS BETWEEN 6 PRECEDING AND CURRENT ROW);";
    sr->ExecuteSQL(base_db, "use " + base_db + ";", &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;
    sr->ExecuteSQL(base_db, deploy_sql, &status);
    ASSERT_TRUE(status.IsOK()) << status.msg;

    PrepareDataForLongWindow(base_db, base_table);
    std::string pre_aggr_db = openmldb::nameserver::PRE_AGG_DB;
    std::string result_sql = "select * from pre_test_aggr_w1_count_i64_col;";
    auto rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    for (int i = 5; i >= 1; i--) {
        ASSERT_TRUE(rs->Next());
        ASSERT_EQ("str1|str2", rs->GetStringUnsafe(0));
        ASSERT_EQ(i * 2 - 1, rs->GetInt64Unsafe(1));
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(2));
        ASSERT_EQ(2, rs->GetInt32Unsafe(3));
        std::string aggr_val_str = rs->GetStringUnsafe(4);
        int64_t aggr_val = *reinterpret_cast<int64_t*>(&aggr_val_str[0]);
        ASSERT_EQ(2, aggr_val);
        ASSERT_EQ(i * 2, rs->GetInt64Unsafe(5));
    }

    result_sql = "select * from pre_test_aggr_w1_count_i16_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_i32_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_f_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_d_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_t_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_s_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    result_sql = "select * from pre_test_aggr_w1_count_date_col;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());
    result_sql = "select * from pre_test_aggr_w1_count_;";
    rs = sr->ExecuteSQL(pre_aggr_db, result_sql, &status);
    ASSERT_EQ(5, rs->Size());

    int req_num = 2;
    for (int i = 0; i < req_num; i++) {
        std::shared_ptr<sdk::SQLRequestRow> req;
        PrepareRequestRowForLongWindow(base_db, "test_aggr", req);
        LOG(WARNING) << "Before CallProcedure";
        auto res = sr->CallProcedure(base_db, "test_aggr", req, &status);
        LOG(WARNING) << "After CallProcedure";
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(1, res->Size());
        ASSERT_TRUE(res->Next());
        ASSERT_EQ("str1", res->GetStringUnsafe(0));
        ASSERT_EQ("str2", res->GetStringUnsafe(1));
        int64_t exp = 7;
        ASSERT_EQ(exp, res->GetInt64Unsafe(2));
        ASSERT_EQ(exp, res->GetInt64Unsafe(3));
        ASSERT_EQ(exp, res->GetInt64Unsafe(4));
        ASSERT_EQ(exp, res->GetInt64Unsafe(5));
        ASSERT_EQ(exp, res->GetInt64Unsafe(6));
        ASSERT_EQ(exp, res->GetInt64Unsafe(7));
        ASSERT_EQ(exp, res->GetInt64Unsafe(8));
        ASSERT_EQ(exp, res->GetInt64Unsafe(9));
        ASSERT_EQ(exp, res->GetInt64Unsafe(10));
        ASSERT_EQ(exp, res->GetInt64Unsafe(11));
    }

    ASSERT_TRUE(cs->GetNsClient()->DropProcedure(base_db, "test_aggr", msg));
    std::string pre_aggr_table = "pre_test_aggr_w1_count_i64_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_i16_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_i32_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_f_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_d_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_t_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_s_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_date_col";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    pre_aggr_table = "pre_test_aggr_w1_count_";
    ok = sr->ExecuteDDL(pre_aggr_db, "drop table " + pre_aggr_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->ExecuteDDL(base_db, "drop table " + base_table + ";", &status);
    ASSERT_TRUE(ok);
    ok = sr->DropDB(base_db, &status);
    ASSERT_TRUE(ok);
}

TEST_P(DBSDKTest, CreateWithoutIndexCol) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test2;");
    HandleSQL("use test2;");
    std::string create_sql =
        "create table trans (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
        "c8 date, index(ts=c7));";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(create_sql, &status);
    ASSERT_TRUE(status.IsOK());
    std::string msg;
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test2", msg));
}

TEST_P(DBSDKTest, CreateIfNotExists) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    HandleSQL("create database test2;");
    HandleSQL("use test2;");
    std::string create_sql = "create table if not exists trans (col1 string);";
    hybridse::sdk::Status status;
    sr->ExecuteSQL(create_sql, &status);
    ASSERT_TRUE(status.IsOK());

    // Run create again and do not get error
    sr->ExecuteSQL(create_sql, &status);
    ASSERT_TRUE(status.IsOK());

    std::string msg;
    ASSERT_TRUE(cs->GetNsClient()->DropTable("test2", "trans", msg));
    ASSERT_TRUE(cs->GetNsClient()->DropDatabase("test2", msg));
}

TEST_P(DBSDKTest, ShowComponents) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQL("show components", &status);
    ASSERT_EQ(status.code, 0);

    if (cs->IsClusterMode()) {
        ASSERT_EQ(3, rs->Size());
        ASSERT_EQ(5, rs->GetSchema()->GetColumnCnt());
        const auto& tablet_eps = mc_->GetTbEndpoint();
        const auto& ns_ep = mc_->GetNsEndpoint();
        ASSERT_EQ(2, tablet_eps.size());
        ExpectResultSetStrEq({{"Endpoint", "Role", "Connect_time", "Status", "Ns_role"},
                              {tablet_eps.at(0), "tablet", {}, "online", "NULL"},
                              {tablet_eps.at(1), "tablet", {}, "online", "NULL"},
                              {ns_ep, "nameserver", {}, "online", "master"}},
                             rs.get(), false);
    } else {
        ASSERT_EQ(2, rs->Size());
        ASSERT_EQ(5, rs->GetSchema()->GetColumnCnt());
        const auto& tablet_ep = env.GetTbEndpoint();
        const auto& ns_ep = env.GetNsEndpoint();
        ExpectResultSetStrEq({{"Endpoint", "Role", "Connect_time", "Status", "Ns_role"},
                              {tablet_ep, "tablet", {}, "online", "NULL"},
                              {ns_ep, "nameserver", {}, "online", "master"}},
                             rs.get());
    }

    HandleSQL("show components");
}

TEST_P(DBSDKTest, ShowTableStatusEmptySet) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    sr->SetDatabase("");

    hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQL("show table status", &status);
    ASSERT_EQ(status.code, 0);
    ExpectResultSetStrEq(
        {
            {"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
             "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
        },
        rs.get());
    HandleSQL("show table status");
}

// show table status when no database is selected
TEST_P(DBSDKTest, ShowTableStatusUnderRoot) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;

    std::string db_name = absl::StrCat("db_", GenRand());
    std::string tb_name = absl::StrCat("tb_", GenRand());

    // prepare data
    ProcessSQLs(sr,
                {
                    "set @@execute_mode = 'online'",
                    absl::StrCat("create database ", db_name, ";"),
                    absl::StrCat("use ", db_name, ";"),
                    absl::StrCat("create table ", tb_name, " (id int, c1 string, c7 timestamp, index(key=id, ts=c7));"),
                    absl::StrCat("insert into ", tb_name, " values (1, 'aaa', 1635247427000);"),
                });
    // reset to empty db
    sr->SetDatabase("");

    // sleep for 4s, name server should updated TableInfo in schedule
    absl::SleepFor(absl::Seconds(4));

    // test
    hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQL("show table status", &status);
    ASSERT_EQ(status.code, 0);
    if (cs->IsClusterMode()) {
        // default partition_num = 8 and replica_num = min(tablet,3) in cluster_mode
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb_name, db_name, "memory", "1", {{}, "0"}, {{}, "0"}, "8", "0", "2", "NULL", "NULL", "NULL"}},
            rs.get());
    } else {
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb_name, db_name, "memory", "1", {{}, "0"}, {{}, "0"}, "1", "0", "1", "NULL", "NULL", "NULL"}},
            rs.get());
    }
    // runs HandleSQL only for the purpose of pretty print result in console
    HandleSQL("show table status");

    // teardown
    ProcessSQLs(sr, {absl::StrCat("use ", db_name), absl::StrCat("drop table ", tb_name),
                     absl::StrCat("drop database ", db_name)});
    sr->SetDatabase("");
}

TEST_P(DBSDKTest, ShowTableStatusForHddTable) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    if (cs->IsClusterMode()) {
        // cluster mode not asserted because of #1695
        // since tablets use of the same gflag to store table data, in mini cluster environment,
        // it lead to dead lock cause tablets runs on same machine
        return;
    }

    std::string db_name = absl::StrCat("db_", GenRand());
    std::string tb_name = absl::StrCat("tb_", GenRand());

    // prepare data
    ProcessSQLs(sr, {
                        "set @@execute_mode = 'online'",
                        absl::StrCat("create database ", db_name, ";"),
                        absl::StrCat("use ", db_name, ";"),
                        absl::StrCat(
                            "create table ", tb_name,
                            " (id int, c1 string, c7 timestamp, index(key=id, ts=c7)) options (storage_mode = 'HDD');"),
                        absl::StrCat("insert into ", tb_name, " values (1, 'aaa', 1635247427000);"),
                    });
    // reset to empty db
    sr->SetDatabase("");

    // sleep for 4s, name server should updated TableInfo in schedule
    absl::SleepFor(absl::Seconds(4));

    // test
    hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQL("show table status", &status);
    ASSERT_EQ(status.code, 0);

    // TODO(ace): Memory_data_size not asserted because not implemented
    ExpectResultSetStrEq(
        {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
          "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
         {{}, tb_name, db_name, "hdd", "1", {}, {{}, "0"}, "1", "0", "1", "NULL", "NULL", "NULL"}},
        rs.get());

    // runs HandleSQL only for the purpose of pretty print result in console
    HandleSQL("show table status");

    // teardown
    ProcessSQLs(sr, {absl::StrCat("use ", db_name), absl::StrCat("drop table ", tb_name),
                     absl::StrCat("drop database ", db_name)});
    sr->SetDatabase("");
}

// show table status after use db
TEST_P(DBSDKTest, ShowTableStatusUnderDB) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;

    std::string db1_name = absl::StrCat("db1_", GenRand());
    std::string tb1_name = absl::StrCat("tb1_", GenRand());
    std::string db2_name = absl::StrCat("db2_", GenRand());
    std::string tb2_name = absl::StrCat("tb2_", GenRand());

    // prepare data
    ProcessSQLs(
        sr, {
                "set @@execute_mode = 'online'",
                absl::StrCat("create database ", db1_name, ";"),
                absl::StrCat("use ", db1_name, ";"),
                absl::StrCat("create table ", tb1_name, " (id int, c1 string, c7 timestamp, index(key=id, ts=c7));"),
                absl::StrCat("insert into ", tb1_name, " values (1, 'aaa', 1635247427000);"),

                absl::StrCat("create database ", db2_name, ";"),
                absl::StrCat("use ", db2_name),
                absl::StrCat("create table ", tb2_name, " (id int, c1 string, c7 timestamp, index(key=id, ts=c7));"),
                absl::StrCat("insert into ", tb2_name, " values (2, 'aaa', 1635247427000);"),
            });

    // sleep for 10s, name server should updated TableInfo in schedule
    absl::SleepFor(absl::Seconds(4));

    // test
    hybridse::sdk::Status status;
    sr->ExecuteSQL(absl::StrCat("use ", db1_name, ";"), &status);
    ASSERT_TRUE(status.IsOK());
    auto rs = sr->ExecuteSQL("show table status", &status);
    ASSERT_EQ(status.code, 0);
    if (cs->IsClusterMode()) {
        // default partition_num = 8 and replica_num = min(tablet,3) in cluster_mode
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb1_name, db1_name, "memory", "1", {{}, "0"}, {{}, "0"}, "8", "0", "2", "NULL", "NULL", "NULL"}},
            rs.get());

        sr->ExecuteSQL(absl::StrCat("use ", db2_name, ";"), &status);
        ASSERT_TRUE(status.IsOK());
        rs = sr->ExecuteSQL("show table status", &status);
        ASSERT_EQ(status.code, 0);
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb2_name, db2_name, "memory", "1", {{}, "0"}, {{}, "0"}, "8", "0", "2", "NULL", "NULL", "NULL"}},
            rs.get());
    } else {
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb1_name, db1_name, "memory", "1", {{}, "0"}, {{}, "0"}, "1", "0", "1", "NULL", "NULL", "NULL"}},
            rs.get());

        sr->ExecuteSQL(absl::StrCat("use ", db2_name, ";"), &status);
        ASSERT_TRUE(status.IsOK());
        rs = sr->ExecuteSQL("show table status", &status);
        ASSERT_EQ(status.code, 0);
        ExpectResultSetStrEq(
            {{"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size", "Disk_data_size",
            "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format", "Offline_deep_copy"},
            {{}, tb2_name, db2_name, "memory", "1", {{}, "0"}, {{}, "0"}, "1", "0", "1", "NULL", "NULL", "NULL"}},
            rs.get());
    }

    // show only tables inside hidden db
    HandleSQL("use INFORMATION_SCHEMA");
    rs = sr->ExecuteSQL("show table status", &status);
    ASSERT_EQ(status.code, 0);
    ExpectResultSetStrEq(
        {
            {"Table_id", "Table_name", "Database_name", "Storage_type", "Rows", "Memory_data_size",
                "Disk_data_size", "Partition", "Partition_unalive", "Replica", "Offline_path", "Offline_format",
                "Offline_deep_copy"},
            {{},
                nameserver::DEPLOY_RESPONSE_TIME,
                nameserver::INFORMATION_SCHEMA_DB,
                "memory",
                {},
                {},
                {},
                "1",
                "0",
                "1",
                "NULL",
                "NULL",
                "NULL"},
            {{},
                nameserver::GLOBAL_VARIABLES,
                nameserver::INFORMATION_SCHEMA_DB,
                "memory",
                "4",
                {},
                {},
                "1",
                "0",
                "1",
                "NULL",
                "NULL",
                "NULL"},
        },
        rs.get());

    // teardown
    ProcessSQLs(sr, {
                        absl::StrCat("use ", db1_name, ";"),
                        absl::StrCat("drop table ", tb1_name),
                        absl::StrCat("drop database ", db1_name),
                        absl::StrCat("use ", db2_name),
                        absl::StrCat("drop table ", tb2_name),
                        absl::StrCat("drop database ", db2_name),
                    });

    sr->SetDatabase("");
}

TEST_P(DBSDKTest, GlobalVariable) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    ProcessSQLs(sr, {
                        "set @@execute_mode='offline';",
                    });

    ::hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQL("show global variables", &status);
    // init global variable
    ExpectResultSetStrEq({{"Variable_name", "Variable_value"},
                          {"enable_trace", "false"},
                          {"sync_job", "false"},
                          {"job_timeout", "20000"},
                          {"execute_mode", "offline"}},
                         rs.get());
    // init session variables from systemtable
    rs = sr->ExecuteSQL("show session variables", &status);
    ExpectResultSetStrEq({{"Variable_name", "Value"},
                          {"enable_trace", "false"},
                          {"execute_mode", "offline"},
                          {"job_timeout", "20000"},
                          {"sync_job", "false"}},
                         rs.get());
    // set global variables
    ProcessSQLs(sr, {
                        "set @@global.enable_trace='true';",
                        "set @@global.sync_job='true';",
                        "set @@global.execute_mode='online';",
                    });
    rs = sr->ExecuteSQL("show global variables", &status);
    ExpectResultSetStrEq({{"Variable_name", "Variable_value"},
                          {"enable_trace", "true"},
                          {"sync_job", "true"},
                          {"job_timeout", "20000"},
                          {"execute_mode", "online"}},
                         rs.get());
    // update session variables if set global variables
    rs = sr->ExecuteSQL("show session variables", &status);
    ExpectResultSetStrEq({{"Variable_name", "Value"},
                          {"enable_trace", "true"},
                          {"execute_mode", "online"},
                          {"job_timeout", "20000"},
                          {"sync_job", "true"}},
                         rs.get());

    ProcessSQLs(sr, {
                        "set @@global.enable_trace='false';",
                        "set @@global.sync_job='false';",
                        "set @@global.execute_mode='offline';",
                    });
    rs = sr->ExecuteSQL("show global variables", &status);
    ExpectResultSetStrEq({{"Variable_name", "Variable_value"},
                          {"enable_trace", "false"},
                          {"sync_job", "false"},
                          {"job_timeout", "20000"},
                          {"execute_mode", "offline"}},
                         rs.get());
}

TEST_P(DBSDKTest, SelectWithAddNewIndex) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;

    std::string db1_name = absl::StrCat("db1_", GenRand());
    std::string tb1_name = absl::StrCat("tb1_", GenRand());

    ProcessSQLs(sr,
                {
                    "set @@execute_mode = 'online'",
                    absl::StrCat("create database ", db1_name, ";"),
                    absl::StrCat("use ", db1_name, ";"),

                    absl::StrCat("create table ", tb1_name,
                                 " (id int, c1 string, c2 int, c3 timestamp, c4 timestamp, "
                                 "index(key=(c1),ts=c4))options(partitionnum=1, replicanum=1);"),
                    absl::StrCat("insert into ", tb1_name, " values(1,'aa',1,1590738990000,1637056523316);"),
                    absl::StrCat("insert into ", tb1_name, " values(2,'bb',1,1590738990000,1637056523316);"),
                    absl::StrCat("insert into ", tb1_name, " values(3,'aa',3,1590738990000,1637057123257);"),
                    absl::StrCat("insert into ", tb1_name, " values(4,'aa',1,1590738990000,1637057123317);"),
                    absl::StrCat("CREATE INDEX index1 ON ", tb1_name, " (c2) OPTIONS (ttl=10m, ttl_type=absolute);"),
                });
    absl::SleepFor(absl::Seconds(4));
    hybridse::sdk::Status status;
    auto res = sr->ExecuteSQL(absl::StrCat("use ", db1_name, ";"), &status);
    res = sr->ExecuteSQL(absl::StrCat("select id,c1,c2,c3 from ", tb1_name), &status);
    ASSERT_EQ(res->Size(), 4);
    res = sr->ExecuteSQL(absl::StrCat("select id,c1,c2,c3 from ", tb1_name, " where c1='aa';"), &status);
    ASSERT_EQ(res->Size(), 3);
    res = sr->ExecuteSQL(absl::StrCat("select id,c1,c2,c3 from ", tb1_name, " where c2=1;"), &status);
    ASSERT_EQ(res->Size(), 3);

    ProcessSQLs(sr, {
                        absl::StrCat("use ", db1_name, ";"),
                        absl::StrCat("drop table ", tb1_name),
                        absl::StrCat("drop database ", db1_name),
                    });

    sr->SetDatabase("");
}

// --------------------------------------------------------------------------------------
// basic functional UTs to test if it is correct for deploy query response time collection
// see NameServerImpl::SyncDeployStats & TabletImpl::TryCollectDeployStats
// --------------------------------------------------------------------------------------

// a proxy class to create and cleanup deployment stats more gracefully
struct DeploymentEnv {
    explicit DeploymentEnv(sdk::SQLClusterRouter* sr) : sr_(sr) {
        db_ = absl::StrCat("db_", absl::Uniform(gen_, 0, std::numeric_limits<int32_t>::max()));
        table_ = absl::StrCat("tb_", absl::Uniform(gen_, 0, std::numeric_limits<int32_t>::max()));
        dp_name_ = absl::StrCat("dp_", absl::Uniform(gen_, 0, std::numeric_limits<int32_t>::max()));
        procedure_name_ = absl::StrCat("procedure_", absl::Uniform(gen_, 0, std::numeric_limits<int32_t>::max()));
    }

    virtual ~DeploymentEnv() {
        TearDown();
    }

    void SetUp() {
        ProcessSQLs(
            sr_,
            {
                "set session execute_mode = 'online'",
                absl::StrCat("create database ", db_),
                absl::StrCat("use ", db_),
                absl::StrCat("create table ", table_,
                             " (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, "
                             "c8 date, index(key=c1, ts=c4, abs_ttl=0, ttl_type=absolute)) "
                             "OPTIONS(partitionnum=1,replicanum=1);"),
                absl::StrCat("deploy ", dp_name_, " SELECT c1, c3, sum(c4) OVER w1 as w1_c4_sum FROM ", table_,
                             " WINDOW w1 AS (PARTITION BY c1 ORDER BY c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW);"),
                absl::StrCat(
                    "create procedure ", procedure_name_,
                    " (c1 string, c3 int, c4 bigint, c5 float, c6 double, c7 timestamp, c8 date) BEGIN SELECT c1, c3, "
                    "sum(c4) OVER w1 as w1_c4_sum FROM ",
                    table_,
                    " WINDOW w1 AS (PARTITION BY c1 ORDER BY c7 ROWS BETWEEN 2 PRECEDING AND CURRENT ROW); END"),
            });
    }

    void TearDown() {
        ProcessSQLs(sr_, {
                             absl::StrCat("drop deployment ", dp_name_),
                             absl::StrCat("drop procedure ", procedure_name_),
                             absl::StrCat("drop table ", table_),
                             absl::StrCat("drop database ", db_),
                             "set global deploy_stats = 'off'",
                         });
    }

    void CallDeployProcedureBatch() {
        hybridse::sdk::Status status;
        std::shared_ptr<sdk::SQLRequestRow> rr = std::make_shared<sdk::SQLRequestRow>();
        GetRequestRow(&rr, dp_name_);
        auto common_column_indices = std::make_shared<sdk::ColumnIndicesSet>(rr->GetSchema());
        auto row_batch = std::make_shared<sdk::SQLRequestRowBatch>(rr->GetSchema(), common_column_indices);
        sr->CallSQLBatchRequestProcedure(db_, dp_name_, row_batch, &status);
        ASSERT_TRUE(status.IsOK()) << status.msg << "\n" << status.trace;
    }

    void CallDeployProcedure() {
        hybridse::sdk::Status status;
        std::shared_ptr<sdk::SQLRequestRow> rr = std::make_shared<sdk::SQLRequestRow>();
        GetRequestRow(&rr, dp_name_);
        sr->CallProcedure(db_, dp_name_, rr, &status);
        ASSERT_TRUE(status.IsOK()) << status.msg << "\n" << status.trace;
    }

    void CallProcedure() {
        hybridse::sdk::Status status;
        std::shared_ptr<sdk::SQLRequestRow> rr = std::make_shared<sdk::SQLRequestRow>();
        GetRequestRow(&rr, procedure_name_);
        sr->CallProcedure(db_, procedure_name_, rr, &status);
        ASSERT_TRUE(status.IsOK()) << status.msg << "\n" << status.trace;
    }

    void EnableDeployStats() {
        ProcessSQLs(sr_, {
                             "set global deploy_stats = 'on'",
                         });
    }

    sdk::SQLClusterRouter* sr_;
    absl::BitGen gen_;
    // variables generate randomly in SetUp
    std::string db_;
    std::string table_;
    std::string dp_name_;
    std::string procedure_name_;

 private:
    void GetRequestRow(std::shared_ptr<sdk::SQLRequestRow>* rs, const std::string& name) { // NOLINT
        hybridse::sdk::Status s;
        auto res = sr_->GetRequestRowByProcedure(db_, name, &s);
        ASSERT_TRUE(s.IsOK());
        ASSERT_TRUE(res->Init(5));
        ASSERT_TRUE(res->AppendString("hello"));
        ASSERT_TRUE(res->AppendInt32(5));
        ASSERT_TRUE(res->AppendInt64(5));
        ASSERT_TRUE(res->AppendFloat(0.1));
        ASSERT_TRUE(res->AppendDouble(0.1));
        ASSERT_TRUE(res->AppendTimestamp(100342));
        ASSERT_TRUE(res->AppendDate(2012, 10, 10));
        ASSERT_TRUE(res->Build());
        *rs = res;
    }
};

static const char QueryDeployResponseTime[] = "select * from INFORMATION_SCHEMA.DEPLOY_RESPONSE_TIME";

TEST_P(DBSDKTest, DeployStatsNotEnableByDefault) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;

    DeploymentEnv env(sr);
    env.SetUp();
    env.CallDeployProcedureBatch();
    env.CallDeployProcedure();

    absl::SleepFor(absl::Seconds(3));

    hybridse::sdk::Status status;
    auto rs = sr->ExecuteSQLParameterized("", QueryDeployResponseTime, {}, &status);
    ASSERT_TRUE(status.IsOK());
    ASSERT_EQ(0, rs->Size());

    env.EnableDeployStats();

    absl::SleepFor(absl::Seconds(3));

    // HandleSQL exists only for purpose of printing
    HandleSQL(QueryDeployResponseTime);
    rs = sr->ExecuteSQLParameterized("", QueryDeployResponseTime, {}, &status);
    ASSERT_TRUE(status.IsOK());
    ASSERT_EQ(0, rs->Size());
}

TEST_P(DBSDKTest, DeployStatsEnabledAfterSetGlobal) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;

    // FIXME(#1547): test skiped due to Deploy Response Time can't enable in standalone mode
    if (cs->IsClusterMode()) {
        DeploymentEnv env(sr);
        env.SetUp();
        env.EnableDeployStats();
        // sleep a while for global variable notification
        absl::SleepFor(absl::Seconds(2));

        hybridse::sdk::Status status;
        auto rs = sr->ExecuteSQLParameterized("", QueryDeployResponseTime, {}, &status);
        ASSERT_TRUE(status.IsOK());
        // as deploy stats in tablet is lazy managed, the deploy stats will stay empty util the first procedure call
        // happens
        ASSERT_EQ(0, rs->Size());

        // warm up deploy stats
        env.CallDeployProcedureBatch();
        env.CallDeployProcedure();

        absl::SleepFor(absl::Seconds(3));

        HandleSQL(QueryDeployResponseTime);
        rs = sr->ExecuteSQLParameterized("", QueryDeployResponseTime, {}, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(TIME_DISTRIBUTION_BUCKET_COUNT, rs->Size());

        int cnt = 0;
        while (rs->Next()) {
            EXPECT_EQ(absl::StrCat(env.db_, ".", env.dp_name_), rs->GetAsStringUnsafe(0));
            cnt += rs->GetInt32Unsafe(2);
        }
        EXPECT_EQ(2, cnt);
    }
}

TEST_P(DBSDKTest, DeployStatsOnlyCollectDeployProcedure) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    if (cs->IsClusterMode()) {
        DeploymentEnv env(sr);
        env.SetUp();

        env.EnableDeployStats();
        absl::SleepFor(absl::Seconds(2));

        for (int i =0; i < 5; ++i) {
            env.CallProcedure();
        }

        for (int i = 0; i < 10; ++i) {
            env.CallDeployProcedureBatch();
            env.CallDeployProcedure();
        }
        absl::SleepFor(absl::Seconds(3));

        HandleSQL(QueryDeployResponseTime);
        hybridse::sdk::Status status;
        auto rs = sr->ExecuteSQLParameterized("", QueryDeployResponseTime, {}, &status);
        ASSERT_TRUE(status.IsOK());
        ASSERT_EQ(TIME_DISTRIBUTION_BUCKET_COUNT, rs->Size());
        int cnt = 0;
        while (rs->Next()) {
            EXPECT_EQ(absl::StrCat(env.db_, ".", env.dp_name_), rs->GetAsStringUnsafe(0));
            cnt += rs->GetInt32Unsafe(2);
        }
        EXPECT_EQ(10 + 10, cnt);
    }
}

class StripSpaceTest : public ::testing::TestWithParam<std::pair<std::string_view, std::string_view>> {};

std::vector<std::pair<std::string_view, std::string_view>> strip_cases = {
                         {"show components;", "show components;"},
                         {"show components;  ", "show components;"},
                         {"show components;\t", "show components;"},
                         {"show components; \t", "show components;"},
                         {"show components; \v\t\r\n\f", "show components;"},
                         {"show components; show", "show components;show"},
};

INSTANTIATE_TEST_SUITE_P(Strip, StripSpaceTest, ::testing::ValuesIn(strip_cases));

TEST_P(StripSpaceTest, Correctness) {
    auto& cs = GetParam();

    std::string output;
    StripStartingSpaceOfLastStmt(cs.first, &output);
    EXPECT_EQ(cs.second, output);
}

/* TODO: Only run test in standalone mode
TEST_P(DBSDKTest, load_data) {
    auto cli = GetParam();
    cs = cli->cs;
    sr = cli->sr;
    std::string read_file_path = "/tmp/data" + GenRand() + ".csv";
    std::string write_file_path = "/tmp/data" + GenRand() + ".csv";
    std::ofstream ofile;
    std::ifstream ifile;
    ofile.open(read_file_path);
    ofile << "1 ---345---567" << std::endl;
    ofile << "1 ---\"3 4 5\"---567" << std::endl;
    ofile << "1 --- -- - --- abc" << std::endl;
    ofile << "1 --- - - --- abc" << std::endl;
    ofile << "1 --- - A --- A--" << std::endl;
    ofile << "1 --- --- -" << std::endl;
    ofile << "1 --- \" --- \" --- A" << std::endl;

    ExecuteSQL("create database test1;");
    ExecuteSQL("use test1;");

    std::string create_sql = "create table t1 (c1 string, c2 string, c3 string);";
    ExecuteSQL(create_sql);

    ExecuteSQL("load data infile '" + read_file_path +
              "' into table t1 OPTIONS( header = false, delimiter = '---', quote = '\"');");
    ExecuteSQL("select * from t1 into outfile '" + write_file_path + "';");

    ifile.open(write_file_path);
    ifile.seekg(0, ifile.end);
    int length = ifile.tellg();
    ifile.seekg(0, ifile.beg);
    char* data = new char[length + 1];
    data[length] = '\0';

    ifile.read(data, length);
    ASSERT_EQ(strcmp(data, "c1,c2,c3\n1, --- ,A\n1,,-\n1,- A,A--\n1,- -,abc\n1,-- -,abc\n1,3 4 5,567\n1,345,567"), 0);
    delete[] data;
    ifile.close();
    ofile.close();
}
*/

INSTANTIATE_TEST_SUITE_P(DBSDK, DBSDKTest, testing::Values(&standalone_cli, &cluster_cli));

}  // namespace cmd
}  // namespace openmldb

int main(int argc, char** argv) {
    ::hybridse::vm::Engine::InitializeGlobalLLVM();
    ::testing::InitGoogleTest(&argc, argv);
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_traverse_cnt_limit = 500;
    FLAGS_zk_session_timeout = 100000;
    // enable disk table flags
    std::filesystem::path tmp_path = std::filesystem::temp_directory_path() / "openmldb";
    absl::Cleanup clean = [&tmp_path]() { std::filesystem::remove_all(tmp_path); };

    const std::string& tmp_path_str = tmp_path.string();
    FLAGS_ssd_root_path = absl::StrCat(tmp_path_str, "/ssd_root_random_", ::openmldb::test::GenRand());
    FLAGS_hdd_root_path = absl::StrCat(tmp_path_str, "/hdd_root_random_", ::openmldb::test::GenRand());
    FLAGS_recycle_bin_hdd_root_path =
        absl::StrCat(tmp_path_str, "/recycle_hdd_root_random_", ::openmldb::test::GenRand());
    FLAGS_recycle_bin_ssd_root_path =
        absl::StrCat(tmp_path_str, "/recycle_ssd_root_random_", ::openmldb::test::GenRand());

    ::openmldb::sdk::MiniCluster mc(6181);
    ::openmldb::cmd::mc_ = &mc;
    FLAGS_enable_distsql = true;
    int ok = ::openmldb::cmd::mc_->SetUp(2);
    sleep(5);
    srand(time(NULL));
    ::openmldb::sdk::ClusterOptions copt;
    copt.zk_cluster = mc.GetZkCluster();
    copt.zk_path = mc.GetZkPath();
    ::openmldb::cmd::cluster_cli.cs = new ::openmldb::sdk::ClusterSDK(copt);
    ::openmldb::cmd::cluster_cli.cs->Init();
    ::openmldb::cmd::cluster_cli.sr = new ::openmldb::sdk::SQLClusterRouter(::openmldb::cmd::cluster_cli.cs);
    ::openmldb::cmd::cluster_cli.sr->Init();

    env.SetUp();
    FLAGS_host = "127.0.0.1";
    FLAGS_port = env.GetNsPort();
    ::openmldb::cmd::standalone_cli.cs = new ::openmldb::sdk::StandAloneSDK(FLAGS_host, FLAGS_port);
    ::openmldb::cmd::standalone_cli.cs->Init();
    ::openmldb::cmd::standalone_cli.sr = new ::openmldb::sdk::SQLClusterRouter(::openmldb::cmd::standalone_cli.cs);
    ::openmldb::cmd::standalone_cli.sr->Init();
    sleep(3);

    ok = RUN_ALL_TESTS();

    // sr owns relative cs
    delete openmldb::cmd::cluster_cli.sr;
    delete openmldb::cmd::standalone_cli.sr;

    mc.Close();
    env.Close();

    return ok;
}
