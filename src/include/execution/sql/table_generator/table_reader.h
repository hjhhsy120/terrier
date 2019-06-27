#pragma once
#include <iostream>
#include <memory>
#include <fstream>
#include <execution/sql/value.h>
#include "csv/csv.h"
#include "type/type_id.h"
#include "catalog/schema.h"
#include "catalog/catalog.h"
#include "transaction/transaction_context.h"
#include "schema_reader.h"
#include "execution/exec/execution_context.h"

namespace tpl::sql {
/**
 * This class reads table from files
 */
class TableReader {
 public:
  /**
   * Constructor
   * @param exec_ctx execution context to use
   */
  explicit TableReader(exec::ExecutionContext * exec_ctx)
  : exec_ctx_{exec_ctx}
  {}

  /**
   * Read a table given a schema file and a data file
   * @param schema_file file containing the schema
   * @param data_file csv file containing the data
   * @return
   */
  uint32_t ReadTable(const std::string & schema_file, const std::string & data_file) {
    uint32_t val_written = 0;
    // Read schema and create table and indexes
    SchemaReader schema_reader(exec_ctx_->GetAccessor());
    auto table_info = schema_reader.ReadTableInfo(schema_file);
    catalog::SqlTableHelper * catalog_table = CreateTable(table_info.get());
    std::vector<catalog::CatalogIndex*> catalog_indexes = CreateIndexes(table_info.get(), catalog_table);

    // Init table projected row
    auto row_pri = catalog_table->GetPRI();
    byte * table_buffer = terrier::common::AllocationUtil::AllocateAligned(
        row_pri->ProjectedRowSize());
    auto table_pr = row_pri->InitializeRow(table_buffer);
    // Init index prs
    std::vector<storage::ProjectedRow*> index_prs;
    for (const auto & catalog_index: catalog_indexes) {
      auto index_pri = catalog_index->GetMetadata()->GetProjectedRowInitializer();
      byte * index_buffer = terrier::common::AllocationUtil::AllocateAligned(
          index_pri.ProjectedRowSize());
      auto index_pr = index_pri.InitializeRow(index_buffer);
      index_prs.emplace_back(index_pr);
    }

    // Iterate through CSV file
    csv::CSVReader reader(data_file);
    for (csv::CSVRow & row: reader) {
      // Write table data
      uint16_t col_idx = 0;
      for (csv::CSVField &field: row) {
        auto col_offset = catalog_table->ColNumToOffset(col_idx);
        auto col_type = table_info->schema->GetColumn(col_idx).GetType();
        WriteTableCol(table_pr, col_offset, col_type, &field);
        col_idx++;
      }
      // Insert into sql table
      auto slot = catalog_table->GetSqlTable()->Insert(exec_ctx_->GetTxn(), *table_pr);
      val_written++;

      // Write index data
      uint32_t index_idx = 0;
      for (const auto &index_info: table_info->indexes) {
        // Get the right catalog info and projected row
        auto catalog_index = catalog_indexes[index_idx];
        auto index_pr = index_prs[index_idx];
        for (u32 index_col_idx = 0; index_col_idx < index_info->schema.size(); index_col_idx++) {
          // Get the offset of this column in the table
          u16 table_offset = catalog_table->ColNumToOffset(index_info->index_map[index_col_idx]);
          // Get the offset of this column in the index
          auto &index_col = catalog_index->GetMetadata()->GetKeySchema()[index_col_idx];
          u16 index_offset =
              static_cast<u16>(catalog_index->GetMetadata()->GetKeyOidToOffsetMap().at(index_col.GetOid()));
          // Check null and write bytes.
          if (index_col.IsNullable() && table_pr->IsNull(table_offset)) {
            index_pr->SetNull(index_offset);
          } else {
            byte *index_data = index_pr->AccessForceNotNull(index_offset);
            std::memcpy(index_data, table_pr->AccessForceNotNull(table_offset),
                        terrier::type::TypeUtil::GetTypeSize(index_col.GetType()));
          }
        }
        // Insert into index
        catalog_index->GetIndex()->Insert(exec_ctx_->GetTxn(), *index_pr, slot);
        index_idx++;
      }
    }

    // Return
    return val_written;
  }

 private:

  catalog::SqlTableHelper * CreateTable(TableInfo * info) {
    auto table_oid = exec_ctx_->GetAccessor()->CreateUserTable(info->table_name, *info->schema);
    return exec_ctx_->GetAccessor()->GetUserTable(table_oid);
  }

  std::vector<catalog::CatalogIndex*> CreateIndexes(TableInfo * info, catalog::SqlTableHelper * catalog_table) {
    std::vector<catalog::CatalogIndex*> results;
    for (const auto & index_info: info->indexes) {
      auto index_oid = exec_ctx_->GetAccessor()->CreateIndex(terrier::storage::index::ConstraintType::DEFAULT,
                            index_info->schema, index_info->index_name);
      auto catalog_index = exec_ctx_->GetAccessor()->GetCatalogIndex(index_oid);
      catalog_index->SetTable(catalog_table->Oid());
      results.emplace_back(catalog_index.get());
    }
    return results;
  }

  void WriteTableCol(storage::ProjectedRow * insert_pr, uint16_t col_offset, type::TypeId type, csv::CSVField * field) {
    if (*field == null_string) {
      insert_pr->SetNull(col_offset);
      std::cout << "setting null" << std::endl;
      return;
    }
    byte * insert_offset = insert_pr->AccessForceNotNull(col_offset);
    switch (type) {
      case type::TypeId::TINYINT: {
        auto val = field->get<int8_t >();
        std::memcpy(insert_offset, &val, sizeof(int8_t));
        break;
      }
      case type::TypeId::SMALLINT: {
        auto val = field->get<int16_t >();
        std::memcpy(insert_offset, &val, sizeof(int16_t));
        break;
      }
      case type::TypeId::INTEGER: {
        auto val = field->get<int32_t>();
        std::memcpy(insert_offset, &val, sizeof(int32_t));
        break;
      }
      case type::TypeId::BIGINT: {
        auto val = field->get<int64_t>();
        std::memcpy(insert_offset, &val, sizeof(int64_t));
        break;
      }
      case type::TypeId::DECIMAL: {
        auto val = field->get<f64>();
        std::memcpy(insert_offset, &val, sizeof(f64));
        break;
      }
      case type::TypeId::DATE: {
        UNREACHABLE("Not implemented in this branch yet");
      }
      case type::TypeId::VARCHAR: {
        auto val = field->get<std::string_view>();
        auto content_size = static_cast<uint32_t>(val.size());
        if (content_size <= storage::VarlenEntry::InlineThreshold()) {
          *reinterpret_cast<storage::VarlenEntry *>(insert_offset) = storage::VarlenEntry::CreateInline(
              reinterpret_cast<const byte*>(val.data()), content_size);
        } else {
          // TODO(Amadou): Use execCtx allocator
          auto content = reinterpret_cast<byte *>(terrier::common::AllocationUtil::AllocateAligned(content_size));
          std::memcpy(content, val.data(), content_size);
          *reinterpret_cast<storage::VarlenEntry *>(insert_offset) = storage::VarlenEntry::Create(content, content_size, true);
        }
        break;
      }
      default:
        UNREACHABLE("Unsupported type. Add it here first!!!");
    }
  }

 private:
  static constexpr const char * null_string = "\\N";
  exec::ExecutionContext * exec_ctx_;

};
}