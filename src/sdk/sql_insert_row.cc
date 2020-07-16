/*
 * sql_insert_row.cc
 * Copyright (C) 4paradigm.com 2020 wangtaize <wangtaize@4paradigm.com>
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

#include "sdk/sql_insert_row.h"

#include <stdint.h>

#include <string>
#include <unordered_map>

#include "base/fe_strings.h"
#include "glog/logging.h"

namespace rtidb {
namespace sdk {

SQLInsertRows::SQLInsertRows(
    std::shared_ptr<::rtidb::nameserver::TableInfo> table_info,
    DefaultValueMap default_map, uint32_t default_str_length)
    : table_info_(table_info),
      default_map_(default_map),
      default_str_length_(default_str_length) {}

std::shared_ptr<SQLInsertRow> SQLInsertRows::NewRow() {
    if (!rows_.empty() && !rows_.back()->IsComplete()) {
        return std::shared_ptr<SQLInsertRow>();
    }
    std::shared_ptr<SQLInsertRow> row = std::make_shared<SQLInsertRow>(
        table_info_, default_map_, default_str_length_);
    rows_.push_back(row);
    return row;
}

SQLInsertRow::SQLInsertRow(
    std::shared_ptr<::rtidb::nameserver::TableInfo> table_info,
    DefaultValueMap default_map, uint32_t default_string_length)
    : table_info_(table_info),
      default_map_(default_map),
      default_string_length_(default_string_length),
      rb_(table_info->column_desc_v1()),
      val_() {
    std::map<std::string, uint32_t> column_name_map;
    uint32_t index_cnt = 0;
    for (int idx = 0; idx < table_info_->column_desc_v1_size(); idx++) {
        if (table_info_->column_desc_v1(idx).is_ts_col()) {
            ts_set_.insert(idx);
        } else if (table_info_->column_desc_v1(idx).add_ts_idx()) {
            index_map_[index_cnt++].push_back(idx);
        }
        column_name_map.insert(
            std::make_pair(table_info_->column_desc_v1(idx).name(), idx));
    }
    if (table_info_->column_key_size() > 0) {
        index_map_.clear();
        for (int idx = 0; idx < table_info_->column_key_size(); ++idx) {
            for (const auto& column : table_info_->column_key(idx).col_name()) {
                index_map_[idx].push_back(column_name_map[column]);
            }
        }
    }
}

bool SQLInsertRow::Init(int str_length) {
    uint32_t row_size = rb_.CalTotalLength(str_length + default_string_length_);
    val_.resize(row_size);
    int8_t* buf_ = reinterpret_cast<int8_t*>(&(val_[0]));
    bool ok = rb_.SetBuffer(reinterpret_cast<int8_t*>(buf_), row_size);
    if (!ok) {
        return false;
    }
    MakeDefault();
    return true;
}

bool SQLInsertRow::PackDimension(const std::string& val) {
    auto index_it = index_map_.find(rb_.GetAppendPos());
    if (index_it != index_map_.end()) {
        raw_dimensions_.insert(std::make_pair(rb_.GetAppendPos(), val));
        return true;
    }
    return false;
}

bool SQLInsertRow::PackTs(uint64_t ts) {
    if (ts_set_.count(rb_.GetAppendPos())) {
        ts_.push_back(ts);
        return true;
    }
    return false;
}

const std::vector<std::pair<std::string, uint32_t>>&
SQLInsertRow::GetDimensions() {
    if (dimensions_.size() > 0) {
        return dimensions_;
    } else {
        for (const auto& kv : index_map_) {
            std::string key;
            for (uint32_t idx : kv.second) {
                if (!key.empty()) {
                    key += "|";
                }
                key += raw_dimensions_[idx];
            }
            dimensions_.push_back(std::make_pair(key, kv.first));
        }
        return dimensions_;
    }
}

bool SQLInsertRow::MakeDefault() {
    auto it = default_map_->find(rb_.GetAppendPos());
    if (it != default_map_->end()) {
        if (it->second->IsNull()) {
            return AppendNULL();
        }
        switch (table_info_->column_desc_v1(rb_.GetAppendPos()).data_type()) {
            case rtidb::type::kBool:
                return AppendBool(it->second->GetInt());
            case rtidb::type::kSmallInt:
                return AppendInt16(it->second->GetSmallInt());
            case rtidb::type::kInt:
                return AppendInt32(it->second->GetInt());
            case rtidb::type::kBigInt:
                return AppendInt64(it->second->GetLong());
            case rtidb::type::kFloat:
                return AppendFloat(it->second->GetFloat());
            case rtidb::type::kDouble:
                return AppendDouble(it->second->GetDouble());
            case rtidb::type::kDate:
                return AppendDate(it->second->GetInt());
            case rtidb::type::kTimestamp:
                return AppendTimestamp(it->second->GetLong());
            case rtidb::type::kVarchar:
            case rtidb::type::kString:
                return AppendString(it->second->GetStr());
            default:
                return false;
        }
    }
    return true;
}

bool SQLInsertRow::AppendBool(bool val) {
    PackDimension(val ? "true" : "false");
    if (rb_.AppendBool(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendInt16(int16_t val) {
    PackDimension(std::to_string(val));
    if (rb_.AppendInt16(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendInt32(int32_t val) {
    PackDimension(std::to_string(val));
    if (rb_.AppendInt32(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendInt64(int64_t val) {
    PackDimension(std::to_string(val));
    PackTs(val);
    if (rb_.AppendInt64(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendTimestamp(int64_t val) {
    PackDimension(std::to_string(val));
    PackTs(val);
    if (rb_.AppendTimestamp(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendFloat(float val) {
    if (rb_.AppendFloat(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendDouble(double val) {
    if (rb_.AppendDouble(val)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendString(const std::string& val) {
    PackDimension(val);
    if (rb_.AppendString(val.c_str(), val.size())) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendString(const char* val, uint32_t length) {
    PackDimension(std::string(val, length));
    if (rb_.AppendString(val, length)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendDate(uint32_t year, uint32_t month, uint32_t day) {
    if (rb_.AppendDate(year, month, day)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendDate(uint32_t date) {
    if (rb_.AppendDate(date)) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::AppendNULL() {
    // todo: deal with null
    if (rb_.AppendNULL()) {
        return MakeDefault();
    }
    return false;
}

bool SQLInsertRow::IsComplete() { return rb_.IsComplete(); }

}  // namespace sdk
}  // namespace rtidb
