/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef SRC_VM_ENGINE_TEST_H_
#define SRC_VM_ENGINE_TEST_H_

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "base/texttable.h"
#include "boost/algorithm/string.hpp"
#include "case/sql_case.h"
#include "codec/fe_row_codec.h"
#include "codec/fe_row_selector.h"
#include "codec/list_iterator_codec.h"
#include "gflags/gflags.h"
#include "gtest/gtest.h"
#include "gtest/internal/gtest-param-util.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "parser/parser.h"
#include "plan/planner.h"
#include "sys/time.h"
#include "vm/engine.h"
#include "vm/test_base.h"
#define MAX_DEBUG_LINES_CNT 20
#define MAX_DEBUG_COLUMN_CNT 20

using namespace llvm;       // NOLINT (build/namespaces)
using namespace llvm::orc;  // NOLINT (build/namespaces)

static const int ENGINE_TEST_RET_SUCCESS = 0;
static const int ENGINE_TEST_RET_INVALID_CASE = 1;
static const int ENGINE_TEST_RET_COMPILE_ERROR = 2;
static const int ENGINE_TEST_RET_EXECUTION_ERROR = 3;
static const int ENGINE_TEST_INIT_CATALOG_ERROR = 4;

namespace fesql {
namespace vm {
using fesql::base::Status;
using fesql::codec::Row;
using fesql::common::kSQLError;
using fesql::sqlcase::SQLCase;
enum EngineRunMode { RUNBATCH, RUNONE };

std::vector<SQLCase> InitCases(std::string yaml_path);
void InitCases(std::string yaml_path, std::vector<SQLCase>& cases);   // NOLINT
void InitCases(std::string yaml_path, std::vector<SQLCase>& cases) {  // NOLINT
    if (!SQLCase::CreateSQLCasesFromYaml(fesql::sqlcase::FindFesqlDirPath(),
                                         yaml_path, cases)) {
        FAIL();
    }
}
int GenerateSqliteTestStringCallback(void* s, int argc, char** argv,
                                     char** azColName);

bool IsNaN(float x);
bool IsNaN(double x);

void CheckSchema(const vm::Schema& schema, const vm::Schema& exp_schema);
void CheckRows(const vm::Schema& schema, const std::vector<Row>& rows,
               const std::vector<Row>& exp_rows);
void PrintRows(const vm::Schema& schema, const std::vector<Row>& rows);

std::string YamlTypeName(type::Type type);
void PrintYamlResult(const vm::Schema& schema, const std::vector<Row>& rows);

const std::vector<Row> SortRows(const vm::Schema& schema,
                                const std::vector<Row>& rows,
                                const std::string& order_col);

const std::string GenerateTableName(int32_t id);

void DoEngineCheckExpect(const SQLCase& sql_case,
                         std::shared_ptr<RunSession> session,
                         const std::vector<Row>& output);
void CheckSQLiteCompatible(const SQLCase& sql_case, const vm::Schema& schema,
                           const std::vector<Row>& output);

class EngineTest : public ::testing::TestWithParam<SQLCase> {
 public:
    EngineTest() {}
    virtual ~EngineTest() {}
};

class BatchRequestEngineTest : public ::testing::TestWithParam<SQLCase> {
 public:
    BatchRequestEngineTest() {}
    virtual ~BatchRequestEngineTest() {}
};

class EngineTestRunner {
 public:
    explicit EngineTestRunner(const SQLCase& sql_case,
                              const EngineOptions options)
        : sql_case_(sql_case), options_(options), engine_() {
        InitSQLCase();
    }
    virtual ~EngineTestRunner() {}

    void SetSession(std::shared_ptr<RunSession> session) { session_ = session; }

    std::shared_ptr<RunSession> GetSession() const { return session_; }

    static Status ExtractTableInfoFromCreateString(
        const std::string& create, SQLCase::TableInfo* table_info);
    Status Compile();
    virtual void InitSQLCase();
    virtual bool InitEngineCatalog() = 0;
    virtual bool InitTable(const std::string table_name) = 0;
    virtual bool AddRowsIntoTable(const std::string table_name,
                                  const std::vector<Row>& rows) = 0;
    virtual bool AddRowIntoTable(const std::string table_name,
                                 const Row& rows) = 0;
    virtual Status PrepareData() = 0;
    virtual Status Compute(std::vector<codec::Row>*) = 0;
    int return_code() const { return return_code_; }
    const SQLCase& sql_case() const { return sql_case_; }

    void RunCheck();
    void RunBenchmark(size_t iters);

 protected:
    SQLCase sql_case_;
    EngineOptions options_;
    std::shared_ptr<Engine> engine_ = nullptr;
    std::shared_ptr<RunSession> session_ = nullptr;
    int return_code_ = ENGINE_TEST_RET_INVALID_CASE;
};
Status EngineTestRunner::ExtractTableInfoFromCreateString(
    const std::string& create, sqlcase::SQLCase::TableInfo* table_info) {
    CHECK_TRUE(table_info != nullptr, common::kNullPointer,
               "Fail extract with null table info");
    CHECK_TRUE(!create.empty(), common::kSQLError,
               "Fail extract with empty create string");

    node::NodeManager manager;
    parser::FeSQLParser parser;
    fesql::plan::NodePointVector trees;
    base::Status status;
    int ret = parser.parse(create, trees, &manager, status);

    if (0 != status.code) {
        std::cout << status << std::endl;
    }
    CHECK_TRUE(0 == ret, common::kSQLError, "Fail to parser SQL");
    //    ASSERT_EQ(1, trees.size());
    //    std::cout << *(trees.front()) << std::endl;
    plan::SimplePlanner planner_ptr(&manager);
    node::PlanNodeList plan_trees;
    CHECK_TRUE(0 == planner_ptr.CreatePlanTree(trees, plan_trees, status),
               common::kPlanError, "Fail to resolve logical plan");
    CHECK_TRUE(1u == plan_trees.size(), common::kPlanError,
               "Fail to extract table info with multi logical plan tree");
    CHECK_TRUE(
        nullptr != plan_trees[0] &&
            node::kPlanTypeCreate == plan_trees[0]->type_,
        common::kPlanError,
        "Fail to extract table info with invalid SQL, CREATE SQL is required");
    node::CreatePlanNode* create_plan =
        dynamic_cast<node::CreatePlanNode*>(plan_trees[0]);
    table_info->name_ = create_plan->GetTableName();
    CHECK_TRUE(create_plan->ExtractColumnsAndIndexs(table_info->columns_,
                                                    table_info->indexs_),
               common::kPlanError, "Invalid Create Plan Node");
    std::ostringstream oss;
    oss << "name: " << table_info->name_ << "\n";
    oss << "columns: [";
    for (auto column : table_info->columns_) {
        oss << column << ",";
    }
    oss << "]\n";
    oss << "indexs: [";
    for (auto index : table_info->indexs_) {
        oss << index << ",";
    }
    oss << "]\n";
    LOG(INFO) << oss.str();
    return Status::OK();
}
void EngineTestRunner::InitSQLCase() {
    for (size_t idx = 0; idx < sql_case_.inputs_.size(); idx++) {
        if (!sql_case_.inputs_[idx].create_.empty()) {
            auto status = ExtractTableInfoFromCreateString(
                sql_case_.inputs_[idx].create_, &sql_case_.inputs_[idx]);
            ASSERT_TRUE(status.isOK()) << status;
        }
    }

    if (!sql_case_.batch_request_.create_.empty()) {
        auto status = ExtractTableInfoFromCreateString(
            sql_case_.batch_request_.create_, &sql_case_.batch_request_);
        ASSERT_TRUE(status.isOK()) << status;
    }
}
Status EngineTestRunner::Compile() {
    std::string sql_str = sql_case_.sql_str();
    for (int j = 0; j < sql_case_.CountInputs(); ++j) {
        std::string placeholder = "{" + std::to_string(j) + "}";
        boost::replace_all(sql_str, placeholder, sql_case_.inputs_[j].name_);
    }
    LOG(INFO) << "Compile SQL:\n" << sql_str;
    CHECK_TRUE(session_ != nullptr, common::kSQLError, "Session is not set");
    if (fesql::sqlcase::SQLCase::IS_DEBUG() || sql_case_.debug()) {
        session_->EnableDebug();
    }
    struct timeval st;
    struct timeval et;
    gettimeofday(&st, nullptr);
    Status status;
    bool ok = engine_->Get(sql_str, sql_case_.db(), *session_, status);
    gettimeofday(&et, nullptr);
    double mill =
        (et.tv_sec - st.tv_sec) * 1000 + (et.tv_usec - st.tv_usec) / 1000.0;
    LOG(INFO) << "SQL Compile take " << mill << " milliseconds";

    if (!ok || !status.isOK()) {
        LOG(INFO) << status.str();
        return_code_ = ENGINE_TEST_RET_COMPILE_ERROR;
    } else {
        LOG(INFO) << "SQL output schema:";
        std::ostringstream oss;
        session_->GetPhysicalPlan()->Print(oss, "");
        LOG(INFO) << "Physical plan:";
        std::cerr << oss.str() << std::endl;

        std::ostringstream runner_oss;
        session_->GetClusterJob().Print(runner_oss, "");
        LOG(INFO) << "Runner plan:";
        std::cerr << runner_oss.str() << std::endl;
    }
    return status;
}

void EngineTestRunner::RunCheck() {
    if (!InitEngineCatalog()) {
        FAIL() << "Engine Test Init Catalog Error";
        return;
    }
    auto engine_mode = session_->engine_mode();
    Status status = Compile();
    ASSERT_EQ(sql_case_.expect().success_, status.isOK());
    if (!status.isOK()) {
        return_code_ = ENGINE_TEST_RET_COMPILE_ERROR;
        return;
    }
    std::ostringstream oss;
    session_->GetPhysicalPlan()->Print(oss, "");
    if (!sql_case_.batch_plan().empty() && engine_mode == kBatchMode) {
        ASSERT_EQ(oss.str(), sql_case_.batch_plan());
    } else if (!sql_case_.cluster_request_plan().empty() &&
               engine_mode == kRequestMode && options_.is_cluster_optimzied()) {
        ASSERT_EQ(oss.str(), sql_case_.cluster_request_plan());
    } else if (!sql_case_.request_plan().empty() &&
               engine_mode == kRequestMode &&
               !options_.is_cluster_optimzied()) {
        ASSERT_EQ(oss.str(), sql_case_.request_plan());
    }
    status = PrepareData();
    ASSERT_TRUE(status.isOK()) << "Prepare data error: " << status;
    if (!status.isOK()) {
        return;
    }
    std::vector<Row> output_rows;
    status = Compute(&output_rows);
    ASSERT_TRUE(status.isOK()) << "Session run error: " << status;
    if (!status.isOK()) {
        return_code_ = ENGINE_TEST_RET_EXECUTION_ERROR;
        return;
    }
    ASSERT_NO_FATAL_FAILURE(
        DoEngineCheckExpect(sql_case_, session_, output_rows));
    return_code_ = ENGINE_TEST_RET_SUCCESS;
}

void EngineTestRunner::RunBenchmark(size_t iters) {
    auto engine_mode = session_->engine_mode();
    if (engine_mode == kRequestMode) {
        LOG(WARNING) << "Request mode case can not properly run many times";
        return;
    }

    Status status = Compile();
    if (!status.isOK()) {
        LOG(WARNING) << "Compile error: " << status;
        return;
    }
    status = PrepareData();
    if (!status.isOK()) {
        LOG(WARNING) << "Prepare data error: " << status;
        return;
    }

    std::vector<Row> output_rows;
    status = Compute(&output_rows);
    if (!status.isOK()) {
        LOG(WARNING) << "Run error: " << status;
        return;
    }
    ASSERT_NO_FATAL_FAILURE(
        DoEngineCheckExpect(sql_case_, session_, output_rows));

    struct timeval st;
    struct timeval et;
    gettimeofday(&st, nullptr);
    for (size_t i = 0; i < iters; ++i) {
        output_rows.clear();
        status = Compute(&output_rows);
        if (!status.isOK()) {
            LOG(WARNING) << "Run error at " << i << "th iter: " << status;
            return;
        }
    }
    gettimeofday(&et, nullptr);
    if (iters != 0) {
        double mill =
            (et.tv_sec - st.tv_sec) * 1000 + (et.tv_usec - st.tv_usec) / 1000.0;
        printf("Engine run take approximately %.5f ms per run\n", mill / iters);
    }
}

class BatchEngineTestRunner : public EngineTestRunner {
 public:
    explicit BatchEngineTestRunner(const SQLCase& sql_case,
                                   const EngineOptions options)
        : EngineTestRunner(sql_case, options) {
        session_ = std::make_shared<BatchRunSession>();
    }
    Status PrepareData() override {
        for (int32_t i = 0; i < sql_case_.CountInputs(); i++) {
            auto input = sql_case_.inputs()[i];
            std::vector<Row> rows;
            sql_case_.ExtractInputData(rows, i);
            size_t repeat = sql_case_.inputs()[i].repeat_;
            if (repeat > 1) {
                size_t row_num = rows.size();
                rows.resize(row_num * repeat);
                size_t offset = row_num;
                for (size_t i = 0; i < repeat - 1; ++i) {
                    std::copy(rows.begin(), rows.begin() + row_num,
                              rows.begin() + offset);
                    offset += row_num;
                }
            }
            if (!rows.empty()) {
                std::string table_name = sql_case_.inputs_[i].name_;
                CHECK_TRUE(AddRowsIntoTable(table_name, rows),
                           common::kSQLError, "Fail to add rows into table ",
                           table_name);
            }
        }
        return Status::OK();
    }

    Status Compute(std::vector<Row>* outputs) override {
        auto batch_session =
            std::dynamic_pointer_cast<BatchRunSession>(session_);
        CHECK_TRUE(batch_session != nullptr, common::kSQLError);
        int run_ret = batch_session->Run(*outputs);
        if (run_ret != 0) {
            return_code_ = ENGINE_TEST_RET_EXECUTION_ERROR;
        }
        CHECK_TRUE(run_ret == 0, common::kSQLError, "Run batch session failed");
        return Status::OK();
    }

    void RunSQLiteCheck() {
        // Determine whether to compare with SQLite
        if (sql_case_.standard_sql() && sql_case_.standard_sql_compatible()) {
            std::vector<Row> output_rows;
            ASSERT_TRUE(Compute(&output_rows).isOK());
            CheckSQLiteCompatible(sql_case_, GetSession()->GetSchema(),
                                  output_rows);
        }
    }
};

class RequestEngineTestRunner : public EngineTestRunner {
 public:
    explicit RequestEngineTestRunner(const SQLCase& sql_case,
                                     const EngineOptions options)
        : EngineTestRunner(sql_case, options) {
        session_ = std::make_shared<RequestRunSession>();
    }

    Status PrepareData() override {
        request_rows_.clear();
        const bool has_batch_request =
            !sql_case_.batch_request_.columns_.empty();
        auto request_session =
            std::dynamic_pointer_cast<RequestRunSession>(session_);
        CHECK_TRUE(request_session != nullptr, common::kSQLError);
        std::string request_name = request_session->GetRequestName();

        if (has_batch_request) {
            CHECK_TRUE(1 <= sql_case_.batch_request_.rows_.size(), kSQLError,
                       "RequestEngine can't handler emtpy rows batch requests");
            CHECK_TRUE(sql_case_.ExtractInputData(sql_case_.batch_request_,
                                                  request_rows_),
                       kSQLError, "Extract case request rows failed");
        }
        for (int32_t i = 0; i < sql_case_.CountInputs(); i++) {
            std::string input_name = sql_case_.inputs_[i].name_;

            if (input_name == request_name && !has_batch_request) {
                CHECK_TRUE(sql_case_.ExtractInputData(request_rows_, i),
                           kSQLError, "Extract case request rows failed");
                continue;
            } else {
                std::vector<Row> rows;
                if (!sql_case_.inputs_[i].rows_.empty() ||
                    !sql_case_.inputs_[i].data_.empty()) {
                    CHECK_TRUE(sql_case_.ExtractInputData(rows, i), kSQLError,
                               "Extract case request rows failed");
                }

                if (sql_case_.inputs()[i].repeat_ > 1) {
                    std::vector<Row> store_rows;
                    for (int64_t j = 0; j < sql_case_.inputs()[i].repeat_;
                         j++) {
                        for (auto row : rows) {
                            store_rows.push_back(row);
                        }
                    }
                    CHECK_TRUE(AddRowsIntoTable(input_name, store_rows),
                               common::kSQLError,
                               "Fail to add rows into table ", input_name);

                } else {
                    CHECK_TRUE(AddRowsIntoTable(input_name, rows),
                               common::kSQLError,
                               "Fail to add rows into table ", input_name);
                }
            }
        }
        return Status::OK();
    }

    Status Compute(std::vector<Row>* outputs) override {
        const bool has_batch_request =
            !sql_case_.batch_request_.columns_.empty() ||
            !sql_case_.batch_request_.schema_.empty();
        auto request_session =
            std::dynamic_pointer_cast<RequestRunSession>(session_);
        std::string request_name = request_session->GetRequestName();
        for (auto in_row : request_rows_) {
            Row out_row;
            int run_ret = request_session->Run(in_row, &out_row);
            if (run_ret != 0) {
                return_code_ = ENGINE_TEST_RET_EXECUTION_ERROR;
                return Status(kSQLError, "Run request session failed");
            }
            if (!has_batch_request) {
                CHECK_TRUE(AddRowIntoTable(request_name, in_row), kSQLError,
                           "Fail add row into table ", request_name);
            }
            outputs->push_back(out_row);
        }
        return Status::OK();
    }

 private:
    std::vector<Row> request_rows_;
};

class BatchRequestEngineTestRunner : public EngineTestRunner {
 public:
    BatchRequestEngineTestRunner(const SQLCase& sql_case,
                                 const EngineOptions options,
                                 const std::set<size_t>& common_column_indices)
        : EngineTestRunner(sql_case, options) {
        auto request_session = std::make_shared<BatchRequestRunSession>();
        for (size_t idx : common_column_indices) {
            request_session->AddCommonColumnIdx(idx);
        }
        session_ = request_session;
    }

    Status PrepareData() override {
        request_rows_.clear();
        auto request_session =
            std::dynamic_pointer_cast<BatchRequestRunSession>(session_);
        CHECK_TRUE(request_session != nullptr, common::kSQLError);

        bool has_batch_request = !sql_case_.batch_request().columns_.empty();
        if (!has_batch_request) {
            LOG(WARNING) << "No batch request field in case, "
                         << "try use last row from primary input";
        }

        std::vector<Row> original_request_data;
        std::string request_name = request_session->GetRequestName();
        auto& request_schema = request_session->GetRequestSchema();
        for (int32_t i = 0; i < sql_case_.CountInputs(); i++) {
            auto input = sql_case_.inputs()[i];
            std::vector<Row> rows;
            sql_case_.ExtractInputData(rows, i);
            if (!rows.empty()) {
                if (sql_case_.inputs_[i].name_ == request_name &&
                    !has_batch_request) {
                    original_request_data.push_back(rows.back());
                    rows.pop_back();
                }
                std::string table_name = sql_case_.inputs_[i].name_;
                size_t repeat = sql_case_.inputs()[i].repeat_;
                if (repeat > 1) {
                    size_t row_num = rows.size();
                    rows.resize(row_num * repeat);
                    size_t offset = row_num;
                    for (size_t i = 0; i < repeat - 1; ++i) {
                        std::copy(rows.begin(), rows.begin() + row_num,
                                  rows.begin() + offset);
                        offset += row_num;
                    }
                }
                CHECK_TRUE(AddRowsIntoTable(table_name, rows),
                           common::kSQLError, "Fail to add rows into table ",
                           table_name);
            }
        }

        type::TableDef request_table;
        if (has_batch_request) {
            sql_case_.ExtractTableDef(sql_case_.batch_request().columns_,
                                      sql_case_.batch_request().indexs_,
                                      request_table);
            sql_case_.ExtractRows(request_table.columns(),
                                  sql_case_.batch_request().rows_,
                                  original_request_data);
        } else {
            sql_case_.ExtractInputTableDef(request_table, 0);
        }

        std::vector<size_t> common_column_indices;
        for (size_t idx : request_session->common_column_indices()) {
            common_column_indices.push_back(idx);
        }
        size_t request_schema_size = static_cast<size_t>(request_schema.size());
        if (common_column_indices.empty() ||
            common_column_indices.size() == request_schema_size ||
            !options_.is_batch_request_optimized()) {
            request_rows_ = original_request_data;
        } else {
            std::vector<size_t> non_common_column_indices;
            for (size_t i = 0; i < request_schema_size; ++i) {
                if (std::find(common_column_indices.begin(),
                              common_column_indices.end(),
                              i) == common_column_indices.end()) {
                    non_common_column_indices.push_back(i);
                }
            }
            codec::RowSelector left_selector(&request_table.columns(),
                                             common_column_indices);
            codec::RowSelector right_selector(&request_table.columns(),
                                              non_common_column_indices);

            bool left_selected = false;
            codec::RefCountedSlice left_slice;
            for (auto& original_row : original_request_data) {
                if (!left_selected) {
                    int8_t* left_buf;
                    size_t left_size;
                    left_selector.Select(original_row.buf(0),
                                         original_row.size(0), &left_buf,
                                         &left_size);
                    left_slice = codec::RefCountedSlice::CreateManaged(
                        left_buf, left_size);
                    left_selected = true;
                }
                int8_t* right_buf = nullptr;
                size_t right_size;
                right_selector.Select(original_row.buf(0), original_row.size(0),
                                      &right_buf, &right_size);
                codec::RefCountedSlice right_slice =
                    codec::RefCountedSlice::CreateManaged(right_buf,
                                                          right_size);
                request_rows_.emplace_back(codec::Row(
                    1, codec::Row(left_slice), 1, codec::Row(right_slice)));
            }
        }
        size_t repeat = sql_case_.batch_request().repeat_;
        if (repeat > 1) {
            size_t row_num = request_rows_.size();
            request_rows_.resize(row_num * repeat);
            size_t offset = row_num;
            for (size_t i = 0; i < repeat - 1; ++i) {
                std::copy(request_rows_.begin(),
                          request_rows_.begin() + row_num,
                          request_rows_.begin() + offset);
                offset += row_num;
            }
        }
        return Status::OK();
    }

    Status Compute(std::vector<Row>* outputs) override {
        auto request_session =
            std::dynamic_pointer_cast<BatchRequestRunSession>(session_);
        CHECK_TRUE(request_session != nullptr, common::kSQLError);

        int run_ret = request_session->Run(request_rows_, *outputs);
        if (run_ret != 0) {
            return_code_ = ENGINE_TEST_RET_EXECUTION_ERROR;
            return Status(kSQLError, "Run batch request session failed");
        }
        return Status::OK();
    }

 private:
    std::vector<Row> request_rows_;
};

}  // namespace vm
}  // namespace fesql
#endif  // SRC_VM_ENGINE_TEST_H_
