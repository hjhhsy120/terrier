#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "catalog/catalog_defs.h"
#include "common/exception.h"
#include "common/json.h"
#include "common/macros.h"
#include "common/sql_node_visitor.h"
#include "parser/parser_defs.h"

namespace terrier {

namespace binder {
class BindNodeVisitor;
}  // namespace binder

namespace parser {

class AbstractExpression;

/**
 * Table location information (Database, Schema, Table).
 */
struct TableInfo {
  /**
   * @param table_name table name
   * @param schema_name schema name
   * @param database_name database name
   */
  TableInfo(std::string table_name, std::string schema_name, std::string database_name)
      : table_name_(std::move(table_name)),
        schema_name_(std::move(schema_name)),
        database_name_(std::move(database_name)) {}

  TableInfo() = default;

  /**
   * @return a copy of the table location information
   */
  std::unique_ptr<TableInfo> Copy() {
    return std::make_unique<TableInfo>(GetTableName(), GetSchemaName(), GetDatabaseName());
  }

  /**
   * @return table name
   */
  std::string GetTableName() { return table_name_; }

  /**
   * @return schema name
   */
  std::string GetSchemaName() { return schema_name_; }

  /**
   * @return database name
   */
  std::string GetDatabaseName() { return database_name_; }

  /**
   * @return TableInfo serialized to json
   */
  nlohmann::json ToJson() const {
    nlohmann::json j;
    j["table_name"] = table_name_;
    j["schema_name"] = schema_name_;
    j["database_name"] = database_name_;
    return j;
  }

  /**
   * @param j json to deserialize
   */
  std::vector<std::unique_ptr<AbstractExpression>> FromJson(const nlohmann::json &j) {
    std::vector<std::unique_ptr<AbstractExpression>> exprs;
    table_name_ = j.at("table_name").get<std::string>();
    schema_name_ = j.at("schema_name").get<std::string>();
    database_name_ = j.at("database_name").get<std::string>();
    return exprs;
  }

 private:
  friend class TableRefStatement;
  friend class TableRef;
  std::string table_name_;
  std::string schema_name_;
  std::string database_name_;

  /**
   * Check if the current table info object has the correct database name.
   * If the current table info does not have a database name, set the database name to the default database name
   * If the current table info has a database name, this function verifies if it matches the defualt database name
   * @param default_database_name Default database name
   */
  void TryBindDatabaseName(const std::string &default_database_name) {
    if (database_name_.empty()) {
      database_name_ = std::string(default_database_name);
    } else if (database_name_ != default_database_name) {
      // TODO(ling): Binder Exception or Parser Exception?
      //    This Exception throw in the binding stage
      throw BINDER_EXCEPTION(("Database " + database_name_ + " in the statement is not the current database.").c_str());
    }
    // TODO(Ling): see if we actual need to set the schema name to any default values
    //  This piece of code comes from pelotn
    //    // if schema name is not specified, then it's default value is "public"
    //    if (table_info_->schema_name.empty())
    //      table_info_->schema_name = DEFAULT_SCHEMA_NAME;
  }
};

DEFINE_JSON_DECLARATIONS(TableInfo);

/**
 * Base class for the parsed SQL statements.
 */
class SQLStatement {
 public:
  /**
   * Create a new SQL statement.
   * @param type SQL statement type
   */
  explicit SQLStatement(StatementType type) : stmt_type_(type) {}

  /**
   * Default constructor for deserialization
   */
  SQLStatement() = default;

  virtual ~SQLStatement() = default;

  /**
   * @return SQL statement type
   */
  virtual StatementType GetType() const { return stmt_type_; }

  // TODO(WAN): do we need this? how is it used?
  // Visitor Pattern used for the optimizer to access statements
  // This allows a facility outside the object itself to determine the type of
  // class using the built-in type system.
  /**
   * Visitor pattern used to access and create optimizer objects.
   * TODO(WAN): this probably can be better described by WEN
   * @param v visitor
   */
  virtual void Accept(SqlNodeVisitor *v) = 0;

  /**
   * @return statement serialized to json
   */
  virtual nlohmann::json ToJson() const {
    nlohmann::json j;
    j["stmt_type"] = stmt_type_;
    return j;
  }

  /**
   * @param j json to deserialize
   */
  virtual std::vector<std::unique_ptr<parser::AbstractExpression>> FromJson(const nlohmann::json &j) {
    std::vector<std::unique_ptr<parser::AbstractExpression>> exprs;
    stmt_type_ = j.at("stmt_type").get<StatementType>();
    return exprs;
  }

 private:
  StatementType stmt_type_;
};

DEFINE_JSON_DECLARATIONS(SQLStatement);

/**
 * Base class for statements that refer to other tables.
 */
class TableRefStatement : public SQLStatement {
 public:
  /**
   * @param type type of SQLStatement being referred to
   * @param table_info table being referred to
   */
  TableRefStatement(const StatementType type, std::unique_ptr<TableInfo> table_info)
      : SQLStatement(type), table_info_(std::move(table_info)) {}

  ~TableRefStatement() override = default;

  /**
   * @return table name
   */
  virtual std::string GetTableName() const { return table_info_->GetTableName(); }

  /**
   * @return table schema name (aka namespace)
   */
  virtual std::string GetSchemaName() const { return table_info_->GetSchemaName(); }

  /**
   * @return database name
   */
  virtual std::string GetDatabaseName() const { return table_info_->GetDatabaseName(); }

 protected:
  /**
   * Check if the current statement has the correct database name.
   * If the current statement does not have a database name, set the database name to the default database name
   * If the current statement has a database name, this function verifies if it matches the defualt database name
   * @param default_database_name Default database name
   */
  void TryBindDatabaseName(const std::string &default_database_name) {
    if (!table_info_) table_info_ = std::make_shared<TableInfo>();
    table_info_->TryBindDatabaseName(default_database_name);
  }

 private:
  friend class binder::BindNodeVisitor;
  std::shared_ptr<TableInfo> table_info_ = nullptr;
};

}  // namespace parser
}  // namespace terrier
