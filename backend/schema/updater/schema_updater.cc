//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "backend/schema/updater/schema_updater.h"

#include <memory>

#include "google/protobuf/repeated_field.h"
#include "zetasql/public/type.h"
#include "absl/status/status.h"
#include "absl/types/optional.h"
#include "backend/datamodel/types.h"
#include "backend/schema/backfills/index_backfill.h"
#include "backend/schema/builders/column_builder.h"
#include "backend/schema/builders/foreign_key_builder.h"
#include "backend/schema/builders/index_builder.h"
#include "backend/schema/builders/table_builder.h"
#include "backend/schema/catalog/column.h"
#include "backend/schema/catalog/index.h"
#include "backend/schema/catalog/table.h"
#include "backend/schema/ddl/operations.pb.h"
#include "backend/schema/graph/schema_graph.h"
#include "backend/schema/graph/schema_graph_editor.h"
#include "backend/schema/parser/DDLParser.h"
#include "backend/schema/parser/ddl_parser.h"
#include "backend/schema/updater/ddl_type_conversion.h"
#include "backend/schema/updater/global_schema_names.h"
#include "backend/schema/updater/schema_validation_context.h"
#include "common/errors.h"
#include "common/limits.h"
#include "zetasql/base/status_macros.h"

namespace google {
namespace spanner {
namespace emulator {
namespace backend {

namespace {

// A class that processes a set of Cloud Spanner DDL statements, and applies
// them to an existing (or empty) `Schema` to obtain the updated `Schema`.
//
// The effects of the DDL statements are checked for semantic validity during
// the process and appropriate errors returned on any violations.
//
// Implementation note:
// Semantic violation checks other than existence checks (required to build
// proper reference relationships in the schema graph) should be avoided in this
// class and should instead be encoded in the `Validate()` and
// `ValidateUpdate()` implementations of `SchemaNode`(s) so that they are
// executed during both database schema creation and update.
class SchemaUpdaterImpl {
 public:
  SchemaUpdaterImpl() = delete;  // Private construction only.
  ~SchemaUpdaterImpl() = default;
  // Disallow copies but enable moves.
  SchemaUpdaterImpl(const SchemaUpdaterImpl&) = delete;
  SchemaUpdaterImpl& operator=(const SchemaUpdaterImpl&) = delete;
  SchemaUpdaterImpl(SchemaUpdaterImpl&&) = default;
  // Assignment move is not supported by absl::Time.
  SchemaUpdaterImpl& operator=(SchemaUpdaterImpl&&) = delete;

  zetasql_base::StatusOr<SchemaUpdaterImpl> static Build(
      zetasql::TypeFactory* type_factory,
      TableIDGenerator* table_id_generator,
      ColumnIDGenerator* column_id_generator, Storage* storage,
      absl::Time schema_change_ts, const Schema* existing_schema) {
    SchemaUpdaterImpl impl(type_factory, table_id_generator,
                           column_id_generator, storage, schema_change_ts,
                           existing_schema);
    ZETASQL_RETURN_IF_ERROR(impl.Init());
    return impl;
  }

  // Apply DDL statements returning the SchemaValidationContext containing
  // the schema change actions resulting from each statement.
  zetasql_base::StatusOr<std::vector<SchemaValidationContext>> ApplyDDLStatements(
      absl::Span<const std::string> statements);

  std::vector<std::unique_ptr<const Schema>> GetIntermediateSchemas() {
    return std::move(intermediate_schemas_);
  }

 private:
  SchemaUpdaterImpl(zetasql::TypeFactory* type_factory,
                    TableIDGenerator* table_id_generator,
                    ColumnIDGenerator* column_id_generator, Storage* storage,
                    absl::Time schema_change_ts, const Schema* existing_schema)
      : type_factory_(type_factory),
        table_id_generator_(table_id_generator),
        column_id_generator_(column_id_generator),
        storage_(storage),
        schema_change_timestamp_(schema_change_ts),
        latest_schema_(existing_schema),
        editor_(nullptr) {}

  // Initializes potentially failing components after construction.
  absl::Status Init();

  // Applies the given `statement` on to `latest_schema_`.
  zetasql_base::StatusOr<std::unique_ptr<const Schema>> ApplyDDLStatement(
      absl::string_view statement);

  // Run any pending schema actions resulting from the schema change statements.
  absl::Status RunPendingActions(
      const std::vector<SchemaValidationContext>& pending_work,
      int* num_succesful);

  // TODO : Add a separate Options object to backend::Column and
  // return that from here.
  zetasql_base::StatusOr<absl::optional<bool>> CreateColumnOptions(
      const ddl::Options& options);

  template <typename ColumnDefModifier>
  absl::Status SetColumnDefinition(const ddl::ColumnDefinition& ddl_column,
                                   ColumnDefModifier* modifier);

  zetasql_base::StatusOr<const Column*> CreateColumn(
      const ddl::ColumnDefinition& ddl_column, const Table* table);

  zetasql_base::StatusOr<const KeyColumn*> CreatePrimaryKeyColumn(
      const ddl::PrimaryKeyConstraint::KeyPart& ddl_key_part,
      const Table* table);

  absl::Status CreatePrimaryKeyConstraint(
      const ddl::PrimaryKeyConstraint& ddl_primary_key,
      Table::Builder* builder);

  absl::Status CreateInterleaveConstraint(
      const ddl::InterleaveConstraint& interleave, Table::Builder* builder);

  absl::Status CreateForeignKeyConstraint(
      const ddl::ForeignKeyConstraint& ddl_foreign_key,
      const Table* referencing_table);

  absl::Status CreateTable(const ddl::CreateTable& ddl_table);

  zetasql_base::StatusOr<const Column*> CreateIndexDataTableColumn(
      const Table* indexed_table, const std::string& source_column_name,
      const Table* index_data_table, bool null_filtered_key_column);

  zetasql_base::StatusOr<std::unique_ptr<const Table>> CreateIndexDataTable(
      const ddl::CreateIndex& ddl_index, const Index* index,
      const Table* indexed_table,
      std::vector<const KeyColumn*>* index_key_columns,
      std::vector<const Column*>* stored_columns);

  absl::Status CreateIndex(const ddl::CreateIndex& ddl_index);

  absl::Status AlterTable(const ddl::AlterTable& alter_table);
  absl::Status AlterInterleave(const ddl::InterleaveConstraint& ddl_interleave,
                               const Table* table);
  absl::Status AddForeignKey(const ddl::ForeignKeyConstraint& ddl_foreign_key,
                             const Table* table);
  absl::Status DropConstraint(const std::string& constraint_name,
                              const Table* table);

  absl::Status DropTable(const ddl::DropTable& drop_table);

  absl::Status DropIndex(const ddl::DropIndex& drop_index);

  // Adds a new schema object `node` to the schema copy being edited by
  // `editor_`.
  absl::Status AddNode(std::unique_ptr<const SchemaNode> node);

  // Drops the schema object `node` in `latest_schema_` from the schema copy
  // being edited by `editor_`.
  absl::Status DropNode(const SchemaNode* node);

  // Modifies the schema object `node` in `LatestSchema` in the schema
  // copy being edited by `editor_`.
  template <typename T>
  absl::Status AlterNode(const T* node,
                         const SchemaGraphEditor::EditCallback<T>& alter_cb) {
    ZETASQL_RET_CHECK_NE(node, nullptr);
    ZETASQL_RETURN_IF_ERROR(editor_->EditNode<T>(node, alter_cb));
    return absl::OkStatus();
  }

  // Type factory for the database. Not owned.
  zetasql::TypeFactory* const type_factory_;

  // Unique table ID generator for the database. Not owned.
  TableIDGenerator* const table_id_generator_;

  // Unique column ID generator for the database. Not owned.
  ColumnIDGenerator* const column_id_generator_;

  // Database's storage. For doing data-dependent validations and index
  // backfills.
  Storage* storage_;

  // The timestamp at which the schema changes should be applied/validated
  // against the database's contents.
  const absl::Time schema_change_timestamp_;

  // The latest schema snapshot corresponding to the statements preceding the
  // statement currently being applied. Note that that does not guarantee that
  // any verfication/backfill effects of those statements have been applied.
  // Not owned.
  const Schema* latest_schema_;

  // The intermediate schema snapshots representing the schema state after
  // applying each statement.
  std::vector<std::unique_ptr<const Schema>> intermediate_schemas_;

  // Validation context for the statement being currently processed.
  SchemaValidationContext* statement_context_;

  // Editor used to modify the schema graph.
  std::unique_ptr<SchemaGraphEditor> editor_;

  // Manages global schema names to prevent and generate unique names.
  GlobalSchemaNames global_names_;
};

absl::Status SchemaUpdaterImpl::Init() {
  for (const SchemaNode* node :
       latest_schema_->GetSchemaGraph()->GetSchemaNodes()) {
    if (auto name = node->GetSchemaNameInfo(); name && name.value().global) {
      ZETASQL_RETURN_IF_ERROR(
          global_names_.AddName(name.value().kind, name.value().name));
    }
  }
  return absl::OkStatus();
}

absl::Status SchemaUpdaterImpl::AddNode(
    std::unique_ptr<const SchemaNode> node) {
  // Since we canonicalize immediately after an edit,
  // we don't expect ay edits while adding a node.
  ZETASQL_RETURN_IF_ERROR(editor_->AddNode(std::move(node)));
  return absl::OkStatus();
}

absl::Status SchemaUpdaterImpl::DropNode(const SchemaNode* node) {
  ZETASQL_RET_CHECK_NE(node, nullptr);
  ZETASQL_RETURN_IF_ERROR(editor_->DeleteNode(node));
  return absl::OkStatus();
}

zetasql_base::StatusOr<std::unique_ptr<const Schema>>
SchemaUpdaterImpl::ApplyDDLStatement(absl::string_view statement) {
  if (statement.empty()) {
    return error::EmptyDDLStatement();
  }

  // Apply the statement to the schema graph.
  ZETASQL_RET_CHECK(!editor_->HasModifications());
  ZETASQL_ASSIGN_OR_RETURN(const auto& ddl_statement,
                   ddl::ParseDDLStatement(statement));

  switch (ddl_statement.kind_case()) {
    case ddl::DDLStatement::kCreateTable: {
      ZETASQL_RETURN_IF_ERROR(CreateTable(ddl_statement.create_table()));
      break;
    }
    case ddl::DDLStatement::kCreateIndex: {
      ZETASQL_RETURN_IF_ERROR(CreateIndex(ddl_statement.create_index()));
      break;
    }
    case ddl::DDLStatement::kAlterTable: {
      ZETASQL_RETURN_IF_ERROR(AlterTable(ddl_statement.alter_table()));
      break;
    }
    case ddl::DDLStatement::kDropTable: {
      ZETASQL_RETURN_IF_ERROR(DropTable(ddl_statement.drop_table()));
      break;
    }
    case ddl::DDLStatement::kDropIndex: {
      ZETASQL_RETURN_IF_ERROR(DropIndex(ddl_statement.drop_index()));
      break;
    }
    default:
      ZETASQL_RET_CHECK(false) << "Unsupported ddl statement: "
                       << ddl_statement.kind_case();
  }
  ZETASQL_ASSIGN_OR_RETURN(auto new_schema_graph, editor_->CanonicalizeGraph());
  return absl::make_unique<const Schema>(std::move(new_schema_graph));
}

zetasql_base::StatusOr<std::vector<SchemaValidationContext>>
SchemaUpdaterImpl::ApplyDDLStatements(
    absl::Span<const std::string> statements) {
  std::vector<SchemaValidationContext> pending_work;

  for (const auto& statement : statements) {
    VLOG(2) << "Applying statement " << statement;
    SchemaValidationContext statement_context{storage_, &global_names_,
                                              schema_change_timestamp_};
    statement_context_ = &statement_context;
    editor_ = absl::make_unique<SchemaGraphEditor>(
        latest_schema_->GetSchemaGraph(), statement_context_);

    // If there is a semantic validation error, then we return right away.
    ZETASQL_ASSIGN_OR_RETURN(auto new_schema, ApplyDDLStatement(statement));

    // We save every schema snapshot as verifiers/backfillers from the
    // current/next statement may need to refer to the previous/current
    // schema snapshots.
    statement_context.SetOldSchemaSnapshot(latest_schema_);
    statement_context.SetNewSchemaSnapshot(new_schema.get());
    latest_schema_ = new_schema.get();
    intermediate_schemas_.emplace_back(std::move(new_schema));

    // If everything was OK, make this the new schema snapshot for processing
    // the next statement and save the pending schema snapshot and backfill
    // work.
    pending_work.emplace_back(std::move(statement_context));
  }

  return pending_work;
}

zetasql_base::StatusOr<absl::optional<bool>> SchemaUpdaterImpl::CreateColumnOptions(
    const ddl::Options& options) {
  absl::optional<bool> allows_commit_timestamp = absl::nullopt;
  for (const ddl::Options::Option& option : options.option_val()) {
    ZETASQL_RET_CHECK_EQ(option.name(), ddl::kCommitTimestampOptionName)
        << "Invalid column option: " << option.name();
    switch (option.kind_case()) {
      case ddl::Options_Option::kBoolValue:
        allows_commit_timestamp = option.bool_value();
        break;
      case ddl::Options_Option::kNullValue:
        allows_commit_timestamp = absl::nullopt;
        break;
      default:
        ZETASQL_RET_CHECK(false) << "Option " << ddl::kCommitTimestampOptionName
                         << " can only take bool_value or null_value.";
    }
  }
  return allows_commit_timestamp;
}

template <typename ColumnDefModifer>
absl::Status SchemaUpdaterImpl::SetColumnDefinition(
    const ddl::ColumnDefinition& ddl_column, ColumnDefModifer* modifier) {
  if (ddl_column.has_properties() &&
      ddl_column.properties().has_column_type()) {
    ZETASQL_ASSIGN_OR_RETURN(const zetasql::Type* column_type,
                     DDLColumnTypeToGoogleSqlType(
                         ddl_column.properties().column_type(), type_factory_));
    modifier->set_type(column_type);
  }
  // Set the default values for nullability and length.
  modifier->set_nullable(true);
  modifier->set_declared_max_length(absl::nullopt);
  for (const ddl::Constraint& ddl_constraint : ddl_column.constraints()) {
    switch (ddl_constraint.kind_case()) {
      case ddl::Constraint::kNotNull: {
        modifier->set_nullable(ddl_constraint.not_null().nullable());
        break;
      }
      case ddl::Constraint::kColumnLength: {
        modifier->set_declared_max_length(
            ddl_constraint.column_length().max_length());
        break;
      }
      default:
        ZETASQL_RET_CHECK(false) << "Unexpected constraint: "
                         << ddl_constraint.kind_case()
                         << " for column: " << ddl_column.column_name();
    }
  }

  if (ddl_column.has_options()) {
    // TODO  : Use a column option builder.
    ZETASQL_ASSIGN_OR_RETURN(auto allows_commit_ts,
                     CreateColumnOptions(ddl_column.options()));
    modifier->set_allow_commit_timestamp(allows_commit_ts);
  }
  return absl::OkStatus();
}

zetasql_base::StatusOr<const Column*> SchemaUpdaterImpl::CreateColumn(
    const ddl::ColumnDefinition& ddl_column, const Table* table) {
  const std::string& column_name = ddl_column.column_name();
  Column::Builder builder;
  builder
      .set_id(column_id_generator_->NextId(
          absl::StrCat(table->Name(), ".", column_name)))
      .set_name(column_name);
  ZETASQL_RETURN_IF_ERROR(SetColumnDefinition(ddl_column, &builder));
  const Column* column = builder.get();
  builder.set_table(table);
  ZETASQL_RETURN_IF_ERROR(AddNode(builder.build()));
  return column;
}

absl::Status SchemaUpdaterImpl::CreateInterleaveConstraint(
    const ddl::InterleaveConstraint& interleave, Table::Builder* builder) {
  auto parent = latest_schema_->FindTable(interleave.parent());
  if (parent == nullptr) {
    const Table* table = builder->get();
    if (table->owner_index() == nullptr) {
      return error::TableNotFound(interleave.parent());
    } else {
      return error::IndexInterleaveTableNotFound(table->owner_index()->Name(),
                                                 interleave.parent());
    }
  }

  ZETASQL_RET_CHECK_EQ(builder->get()->parent(), nullptr);

  ZETASQL_RETURN_IF_ERROR(AlterNode<Table>(
      parent, [builder](Table::Editor* parent_editor) -> absl::Status {
        parent_editor->add_child_table(builder->get());
        builder->set_parent_table(parent_editor->get());
        return absl::OkStatus();
      }));

  if (interleave.on_delete().action() == ddl::OnDeleteAction::CASCADE) {
    builder->set_on_delete(Table::OnDeleteAction::kCascade);
  } else {
    builder->set_on_delete(Table::OnDeleteAction::kNoAction);
  }

  return absl::OkStatus();
}

absl::Status SchemaUpdaterImpl::CreatePrimaryKeyConstraint(
    const ddl::PrimaryKeyConstraint& ddl_primary_key, Table::Builder* builder) {
  for (const ddl::PrimaryKeyConstraint::KeyPart& ddl_key_part :
       ddl_primary_key.key_part()) {
    ZETASQL_ASSIGN_OR_RETURN(const KeyColumn* key_col,
                     CreatePrimaryKeyColumn(ddl_key_part, builder->get()));
    builder->add_key_column(key_col);
  }
  return absl::OkStatus();
}

zetasql_base::StatusOr<const KeyColumn*> SchemaUpdaterImpl::CreatePrimaryKeyColumn(
    const ddl::PrimaryKeyConstraint::KeyPart& ddl_key_part,
    const Table* table) {
  KeyColumn::Builder builder;
  const std::string& key_column_name = ddl_key_part.key_column_name();
  bool is_descending =
      (ddl_key_part.order() == ddl::PrimaryKeyConstraint::DESC);

  // References to columns in primary key clause are case-sensitive.
  const Column* column = table->FindColumnCaseSensitive(key_column_name);
  if (column == nullptr) {
    return error::NonExistentKeyColumn(
        OwningObjectType(table), OwningObjectName(table), key_column_name);
  }
  builder.set_column(column).set_descending(is_descending);
  const KeyColumn* key_col = builder.get();
  ZETASQL_RETURN_IF_ERROR(AddNode(builder.build()));
  return key_col;
}

absl::Status SchemaUpdaterImpl::CreateForeignKeyConstraint(
    const ddl::ForeignKeyConstraint& ddl_foreign_key,
    const Table* referencing_table) {
  // TODO Add backing indexes.
  // TODO Validate existing data.
  // TODO Remove warning once enforcement is added.
  VLOG(0) << "Foreign keys are under development and not yet enforced.";
  ForeignKey::Builder foreign_key_builder;

  ZETASQL_RETURN_IF_ERROR(AlterNode<Table>(
      referencing_table, [&](Table::Editor* referencing_table_editor) {
        referencing_table = referencing_table_editor->get();
        referencing_table_editor->add_foreign_key(foreign_key_builder.get());
        return absl::OkStatus();
      }));
  foreign_key_builder.set_referencing_table(referencing_table);

  const Table* referenced_table = latest_schema_->FindTableCaseSensitive(
      ddl_foreign_key.referenced_table_name());
  if (referenced_table == nullptr) {
    if (ddl_foreign_key.referenced_table_name() != referencing_table->Name()) {
      return error::TableNotFound(ddl_foreign_key.referenced_table_name());
    }
    // Self-referencing foreign key.
    referenced_table = referencing_table;
  }
  ZETASQL_RETURN_IF_ERROR(AlterNode<Table>(
      referenced_table, [&](Table::Editor* referenced_table_editor) {
        referenced_table = referenced_table_editor->get();
        referenced_table_editor->add_referencing_foreign_key(
            foreign_key_builder.get());
        return absl::OkStatus();
      }));
  foreign_key_builder.set_referenced_table(referenced_table);

  std::string foreign_key_name;
  if (ddl_foreign_key.has_constraint_name()) {
    foreign_key_name = ddl_foreign_key.constraint_name();
    ZETASQL_RETURN_IF_ERROR(global_names_.AddName("Foreign Key", foreign_key_name));
    foreign_key_builder.set_constraint_name(foreign_key_name);
  } else {
    ZETASQL_ASSIGN_OR_RETURN(foreign_key_name,
                     global_names_.GenerateForeignKeyName(
                         referencing_table->Name(), referenced_table->Name()));
    foreign_key_builder.set_generated_name(foreign_key_name);
  }

  auto add_columns = [&](const Table* table,
                         google::protobuf::RepeatedPtrField<std::string> column_names,
                         std::function<void(const Column*)> add_column) {
    for (const std::string& column_name : column_names) {
      const Column* column = table->FindColumnCaseSensitive(column_name);
      if (column == nullptr) {
        return error::ForeignKeyColumnNotFound(column_name, table->Name(),
                                               foreign_key_name);
      }
      add_column(column);
    }
    return absl::OkStatus();
  };
  ZETASQL_RETURN_IF_ERROR(
      add_columns(referencing_table, ddl_foreign_key.referencing_column_name(),
                  [&foreign_key_builder](const Column* column) {
                    foreign_key_builder.add_referencing_column(column);
                  }));
  ZETASQL_RETURN_IF_ERROR(
      add_columns(referenced_table, ddl_foreign_key.referenced_column_name(),
                  [&foreign_key_builder](const Column* column) {
                    foreign_key_builder.add_referenced_column(column);
                  }));

  return AddNode(foreign_key_builder.build());
}

absl::Status SchemaUpdaterImpl::CreateTable(const ddl::CreateTable& ddl_table) {
  if (latest_schema_->tables().size() >= limits::kMaxTablesPerDatabase) {
    return error::TooManyTablesPerDatabase(ddl_table.table_name(),
                                           limits::kMaxTablesPerDatabase);
  }

  ZETASQL_RETURN_IF_ERROR(global_names_.AddName("Table", ddl_table.table_name()));

  Table::Builder builder;
  builder.set_id(table_id_generator_->NextId(ddl_table.table_name()))
      .set_name(ddl_table.table_name());

  for (const ddl::ColumnDefinition& ddl_column : ddl_table.columns()) {
    ZETASQL_ASSIGN_OR_RETURN(const Column* column,
                     CreateColumn(ddl_column, builder.get()));
    builder.add_column(column);
  }

  for (const ddl::Constraint& ddl_constraint : ddl_table.constraints()) {
    switch (ddl_constraint.kind_case()) {
      case ddl::Constraint::kPrimaryKey: {
        ZETASQL_RETURN_IF_ERROR(
            CreatePrimaryKeyConstraint(ddl_constraint.primary_key(), &builder));
        break;
      }
      case ddl::Constraint::kInterleave: {
        ZETASQL_RETURN_IF_ERROR(
            CreateInterleaveConstraint(ddl_constraint.interleave(), &builder));
        break;
      }
      case ddl::Constraint::kForeignKey: {
        ZETASQL_RETURN_IF_ERROR(CreateForeignKeyConstraint(ddl_constraint.foreign_key(),
                                                   builder.get()));
        break;
      }
      default:
        ZETASQL_RET_CHECK(false) << "Unsupported constraint type: "
                         << ddl_constraint.DebugString();
    }
  }

  ZETASQL_RETURN_IF_ERROR(AddNode(builder.build()));
  return absl::OkStatus();
}

zetasql_base::StatusOr<const Column*> SchemaUpdaterImpl::CreateIndexDataTableColumn(
    const Table* indexed_table, const std::string& source_column_name,
    const Table* index_data_table, bool null_filtered_key_column) {
  const Column* source_column = indexed_table->FindColumn(source_column_name);
  if (source_column == nullptr) {
    return error::IndexRefsNonExistentColumn(
        index_data_table->owner_index()->Name(), source_column_name);
  }

  Column::Builder builder;
  builder.set_name(source_column->Name())
      .set_id(column_id_generator_->NextId(
          absl::StrCat(index_data_table->Name(), ".", source_column->Name())))
      .set_source_column(source_column)
      .set_table(index_data_table);

  if (null_filtered_key_column) {
    builder.set_nullable(false);
  } else {
    builder.set_nullable(source_column->is_nullable());
  }

  const Column* column = builder.get();
  ZETASQL_RETURN_IF_ERROR(AddNode(builder.build()));
  return column;
}

zetasql_base::StatusOr<std::unique_ptr<const Table>>
SchemaUpdaterImpl::CreateIndexDataTable(
    const ddl::CreateIndex& ddl_index, const Index* index,
    const Table* indexed_table,
    std::vector<const KeyColumn*>* index_key_columns,
    std::vector<const Column*>* stored_columns) {
  std::string table_name =
      absl::StrCat(kIndexDataTablePrefix, ddl_index.index_name());
  Table::Builder builder;
  builder.set_name(table_name)
      .set_id(table_id_generator_->NextId(table_name))
      .set_owner_index(index);

  // Add indexed columns to the index_data_table's columns and primary key.
  for (const ddl::Constraint& ddl_constraint : ddl_index.constraints()) {
    switch (ddl_constraint.kind_case()) {
      case ddl::Constraint::kPrimaryKey: {
        const ddl::PrimaryKeyConstraint ddl_primary_key =
            ddl_constraint.primary_key();
        // The primary key is a combination of (index_keys,indexed_table_keys)
        ddl::PrimaryKeyConstraint data_table_pk = ddl_primary_key;

        // First create columns for the specified primary key.
        for (const ddl::PrimaryKeyConstraint::KeyPart& ddl_key_part :
             ddl_primary_key.key_part()) {
          const std::string& column_name = ddl_key_part.key_column_name();
          ZETASQL_ASSIGN_OR_RETURN(const Column* column,
                           CreateIndexDataTableColumn(
                               indexed_table, column_name, builder.get(),
                               index->is_null_filtered()));
          builder.add_column(column);
        }

        // Next, create columns for the indexed table's primary key.
        for (const KeyColumn* key_col : indexed_table->primary_key()) {
          if (builder.get()->FindColumn(key_col->column()->Name()) != nullptr) {
            // Skip already added columns
            continue;
          }
          std::string key_col_name = key_col->column()->Name();
          ZETASQL_ASSIGN_OR_RETURN(const Column* column,
                           CreateIndexDataTableColumn(
                               indexed_table, key_col_name, builder.get(),
                               index->is_null_filtered()));
          builder.add_column(column);

          // Add to the PK specification.
          ddl::PrimaryKeyConstraint::KeyPart* key_part =
              data_table_pk.add_key_part();
          key_part->set_key_column_name(key_col_name);
          if (key_col->is_descending()) {
            key_part->set_order(ddl::PrimaryKeyConstraint::DESC);
          }
        }

        ZETASQL_RETURN_IF_ERROR(CreatePrimaryKeyConstraint(data_table_pk, &builder));
        int num_declared_keys = ddl_primary_key.key_part_size();
        auto data_table_key_cols = builder.get()->primary_key();
        for (int i = 0; i < num_declared_keys; ++i) {
          index_key_columns->push_back(data_table_key_cols[i]);
        }
        break;
      }
      case ddl::Constraint::kInterleave: {
        auto interleave_constraint = ddl_constraint.interleave();
        interleave_constraint.mutable_on_delete()->set_action(
            ddl::OnDeleteAction::CASCADE);
        ZETASQL_RETURN_IF_ERROR(
            CreateInterleaveConstraint(interleave_constraint, &builder));
        break;
      }
      default:
        ZETASQL_RET_CHECK(false) << "Unsupported constraint type: "
                         << ddl_constraint.DebugString();
    }
  }

  // Add stored columns to index data table.
  for (const ddl::ColumnDefinition& ddl_column : ddl_index.columns()) {
    ZETASQL_RET_CHECK(ddl_column.has_properties() &&
              ddl_column.properties().has_stored() &&
              ddl_column.properties().stored() == ddl_column.column_name())
        << "Invalid stored column specification for index: "
        << ddl_index.DebugString() << " " << ddl_column.DebugString();
    const std::string& column_name = ddl_column.column_name();
    ZETASQL_ASSIGN_OR_RETURN(
        const Column* column,
        CreateIndexDataTableColumn(indexed_table, column_name, builder.get(),
                                   /*null_filtered_key_column=*/false));
    builder.add_column(column);
    stored_columns->push_back(column);
  }

  return builder.build();
}

absl::Status SchemaUpdaterImpl::CreateIndex(const ddl::CreateIndex& ddl_index) {
  auto indexed_table = latest_schema_->FindTable(ddl_index.table_name());
  if (indexed_table == nullptr) {
    return error::TableNotFound(ddl_index.table_name());
  }

  if (latest_schema_->num_index() >= limits::kMaxIndexesPerDatabase) {
    return error::TooManyIndicesPerDatabase(ddl_index.index_name(),
                                            limits::kMaxIndexesPerDatabase);
  }

  // Tables and indexes share a namespace.
  ZETASQL_RETURN_IF_ERROR(global_names_.AddName("Index", ddl_index.index_name()));

  Index::Builder builder;
  builder.set_name(ddl_index.index_name())
      .set_unique(ddl_index.properties().unique())
      .set_null_filtered(ddl_index.properties().null_filtered());

  std::vector<const KeyColumn*> key_columns;
  std::vector<const Column*> stored_columns;
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const Table> data_table,
                   CreateIndexDataTable(ddl_index, builder.get(), indexed_table,
                                        &key_columns, &stored_columns));
  builder.set_index_data_table(data_table.get());

  for (const KeyColumn* key_col : key_columns) {
    builder.add_key_column(key_col);
  }

  for (const Column* col : stored_columns) {
    builder.add_stored_column(col);
  }

  ZETASQL_RETURN_IF_ERROR(AlterNode<Table>(
      indexed_table, [&builder](Table::Editor* table_editor) -> absl::Status {
        table_editor->add_index(builder.get());
        builder.set_indexed_table(table_editor->get());
        return absl::OkStatus();
      }));

  // Register a backfill action for the index.
  const Index* index = builder.get();
  statement_context_->AddAction(
      [index](const SchemaValidationContext* context) {
        return BackfillIndex(index, context);
      });

  // The data table must be added after the index for correct order of
  // validation.
  ZETASQL_RETURN_IF_ERROR(AddNode(builder.build()));
  ZETASQL_RETURN_IF_ERROR(AddNode(std::move(data_table)));
  return absl::OkStatus();
}

absl::Status SchemaUpdaterImpl::AlterTable(const ddl::AlterTable& alter_table) {
  const Table* table = latest_schema_->FindTable(alter_table.table_name());
  if (table == nullptr) {
    return error::TableNotFound(alter_table.table_name());
  }

  ZETASQL_RET_CHECK(alter_table.has_alter_column() ||
            alter_table.has_alter_constraint());

  if (alter_table.has_alter_constraint()) {
    const auto& alter_constraint = alter_table.alter_constraint();
    auto alter_type = alter_constraint.type();
    if (alter_constraint.constraint().has_interleave() &&
        alter_type == ddl::AlterConstraint::ALTER) {
      return AlterInterleave(alter_constraint.constraint().interleave(), table);
    }
    if (alter_constraint.constraint().has_foreign_key() &&
        alter_type == ddl::AlterConstraint::ADD) {
      return AddForeignKey(alter_constraint.constraint().foreign_key(), table);
    }
    if (!alter_constraint.has_constraint() &&
        alter_constraint.has_constraint_name() &&
        alter_type == ddl::AlterConstraint::DROP) {
      return DropConstraint(alter_constraint.constraint_name(), table);
    }
    return error::Internal(
        absl::StrCat("Invalid alter table constraint operation: ",
                     alter_table.DebugString()));
  }

  if (alter_table.has_alter_column()) {
    const auto& alter_column = alter_table.alter_column();
    switch (alter_column.type()) {
      case ddl::AlterColumn::ADD: {
        const auto& column_def = alter_column.column();
        ZETASQL_ASSIGN_OR_RETURN(const Column* new_column,
                         CreateColumn(column_def, table));
        ZETASQL_RETURN_IF_ERROR(AlterNode<Table>(
            table, [new_column](Table::Editor* editor) -> absl::Status {
              editor->add_column(new_column);
              return absl::OkStatus();
            }));
        break;
      }
      case ddl::AlterColumn::ALTER: {
        const Column* column = table->FindColumn(alter_column.column_name());
        if (column == nullptr) {
          return error::ColumnNotFound(table->Name(),
                                       alter_column.column_name());
        }
        const auto& column_def = alter_column.column();
        ZETASQL_RETURN_IF_ERROR(AlterNode<Column>(
            column,
            [this, &column_def](Column::Editor* editor) -> absl::Status {
              ZETASQL_RETURN_IF_ERROR(SetColumnDefinition(column_def, editor));
              return absl::OkStatus();
            }));
        break;
      }
      case ddl::AlterColumn::DROP: {
        const Column* column = table->FindColumn(alter_column.column_name());
        if (column == nullptr) {
          return error::ColumnNotFound(table->Name(),
                                       alter_column.column_name());
        }
        ZETASQL_RETURN_IF_ERROR(DropNode(column));
        break;
      }
      default:
        ZETASQL_RET_CHECK(false) << "Invalid alter column specification: "
                         << alter_column.DebugString();
    }
    return absl::OkStatus();
  }

  return absl::OkStatus();
}

absl::Status SchemaUpdaterImpl::AlterInterleave(
    const ddl::InterleaveConstraint& ddl_interleave, const Table* table) {
  return AlterNode<Table>(table, [&](Table::Editor* editor) {
    if (ddl_interleave.on_delete().action() == ddl::OnDeleteAction::CASCADE) {
      editor->set_on_delete(Table::OnDeleteAction::kCascade);
    } else {
      editor->set_on_delete(Table::OnDeleteAction::kNoAction);
    }
    return absl::OkStatus();
  });
}

absl::Status SchemaUpdaterImpl::AddForeignKey(
    const ddl::ForeignKeyConstraint& ddl_foreign_key, const Table* table) {
  return AlterNode<Table>(table, [&](Table::Editor* editor) -> absl::Status {
    return CreateForeignKeyConstraint(ddl_foreign_key, table);
  });
}

absl::Status SchemaUpdaterImpl::DropConstraint(
    const std::string& constraint_name, const Table* table) {
  // Try each type of constraint supported by ALTER TABLE DROP CONSTRAINT.
  const ForeignKey* foreign_key = table->FindForeignKey(constraint_name);
  if (foreign_key != nullptr) {
    return DropNode(foreign_key);
  }
  return error::ConstraintNotFound(constraint_name, table->Name());
}

absl::Status SchemaUpdaterImpl::DropTable(const ddl::DropTable& drop_table) {
  const Table* table = latest_schema_->FindTable(drop_table.table_name());
  if (table == nullptr) {
    return error::TableNotFound(drop_table.table_name());
  }
  return DropNode(table);
}

absl::Status SchemaUpdaterImpl::DropIndex(const ddl::DropIndex& drop_index) {
  const Index* index = latest_schema_->FindIndex(drop_index.index_name());
  if (index == nullptr) {
    return error::IndexNotFound(drop_index.index_name());
  }
  return DropNode(index);
}

}  // namespace

const Schema* SchemaUpdater::EmptySchema() {
  static const Schema* empty_schema = new Schema;
  return empty_schema;
}

zetasql_base::StatusOr<std::unique_ptr<const Schema>>
SchemaUpdater::ValidateSchemaFromDDL(absl::Span<const std::string> statements,
                                     const SchemaChangeContext& context,
                                     const Schema* existing_schema) {
  if (existing_schema == nullptr) {
    existing_schema = EmptySchema();
  }
  ZETASQL_ASSIGN_OR_RETURN(SchemaUpdaterImpl updater,
                   SchemaUpdaterImpl::Build(
                       context.type_factory, context.table_id_generator,
                       context.column_id_generator, context.storage,
                       context.schema_change_timestamp, existing_schema));
  ZETASQL_ASSIGN_OR_RETURN(pending_work_, updater.ApplyDDLStatements(statements));
  intermediate_schemas_ = updater.GetIntermediateSchemas();

  std::unique_ptr<const Schema> new_schema = nullptr;
  if (!intermediate_schemas_.empty()) {
    new_schema = std::move(*intermediate_schemas_.rbegin());
  }
  pending_work_.clear();
  intermediate_schemas_.clear();
  return new_schema;
}

// TODO : These should run in a ReadWriteTransaction with rollback
// capability so that changes to the database can be reversed.
absl::Status SchemaUpdater::RunPendingActions(int* num_succesful) {
  for (const auto& pending_statement : pending_work_) {
    ZETASQL_RETURN_IF_ERROR(pending_statement.RunSchemaChangeActions());
    ++(*num_succesful);
  }
  return absl::OkStatus();
}

zetasql_base::StatusOr<SchemaChangeResult> SchemaUpdater::UpdateSchemaFromDDL(
    const Schema* existing_schema, absl::Span<const std::string> statements,
    const SchemaChangeContext& context) {
  ZETASQL_ASSIGN_OR_RETURN(SchemaUpdaterImpl updater,
                   SchemaUpdaterImpl::Build(
                       context.type_factory, context.table_id_generator,
                       context.column_id_generator, context.storage,
                       context.schema_change_timestamp, existing_schema));
  ZETASQL_ASSIGN_OR_RETURN(pending_work_, updater.ApplyDDLStatements(statements));
  intermediate_schemas_ = updater.GetIntermediateSchemas();

  // Use the schema snapshot for the last succesful statement.
  int num_successful = 0;
  std::unique_ptr<const Schema> new_schema = nullptr;

  absl::Status backfill_status = RunPendingActions(&num_successful);
  if (num_successful > 0) {
    new_schema = std::move(intermediate_schemas_[num_successful - 1]);
  }
  ZETASQL_RET_CHECK_LE(num_successful, intermediate_schemas_.size());
  return SchemaChangeResult{
      .num_successful_statements = num_successful,
      .updated_schema = std::move(new_schema),
      .backfill_status = backfill_status,
  };
}

zetasql_base::StatusOr<std::unique_ptr<const Schema>>
SchemaUpdater::CreateSchemaFromDDL(absl::Span<const std::string> statements,
                                   const SchemaChangeContext& context) {
  ZETASQL_ASSIGN_OR_RETURN(SchemaChangeResult result,
                   UpdateSchemaFromDDL(EmptySchema(), statements, context));
  ZETASQL_RETURN_IF_ERROR(result.backfill_status);
  return std::move(result.updated_schema);
}

}  // namespace backend
}  // namespace emulator
}  // namespace spanner
}  // namespace google
