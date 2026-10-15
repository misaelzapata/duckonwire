#include "duckonwire/native_access.hpp"
#include "duckonwire/peer_guard.hpp"
#include "duckonwire/scram.hpp"
#include "quack_security.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/list.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <array>
#include <cstring>
#include <functional>
#include <utility>

namespace duckonwire {
namespace {
std::string Upper(std::string value) {
    for (char &c : value) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return value;
}
bool FilenameReference(const duckdb::Identifier &identifier) {
    if (identifier.empty()) return false;
    const auto name = Upper(identifier.GetIdentifierName());
    if (name.find_first_of("/\\:*?[]") != std::string::npos) return true;
    const std::array<const char *, 7> extensions {{".CSV", ".TSV", ".PARQUET", ".JSON", ".JSONL", ".NDJSON", ".DUCKDB"}};
    for (const auto extension : extensions) {
        const auto at = name.rfind(extension);
        if (at == std::string::npos) continue;
        const auto after = name.substr(at + std::strlen(extension));
        if (after.empty() || after == ".GZ" || after == ".ZST" || after == ".ZSTD") return true;
    }
    return false;
}
void GuardFileReferences(duckdb::SQLStatement &statement) {
    std::function<void(duckdb::QueryNode &)> query;
    std::function<void(duckdb::ParsedExpression &)> expression;
    std::function<void(duckdb::TableRef &)> reference;
    expression = [&](duckdb::ParsedExpression &expr) {
        if (expr.GetExpressionClass() == duckdb::ExpressionClass::SUBQUERY) {
            auto &subquery = expr.Cast<duckdb::SubqueryExpression>().SubqueryMutable();
            if (subquery) query(*subquery->node);
        }
        duckdb::ParsedExpressionIterator::EnumerateChildren(expr, [&](duckdb::ParsedExpression &child) { expression(child); });
    };
    const auto name_guard = [&](const duckdb::QualifiedName &name) {
        if (FilenameReference(name.Catalog()) || FilenameReference(name.Schema()) || FilenameReference(name.Name()))
            throw duckdb::PermissionException("File references and replacement scans are not allowed on the published database");
    };
    reference = [&](duckdb::TableRef &ref) {
        if (ref.type == duckdb::TableReferenceType::BASE_TABLE) name_guard(ref.Cast<duckdb::BaseTableRef>().GetQualifiedName());
        else if (ref.type == duckdb::TableReferenceType::TABLE_FUNCTION) {
            auto &subquery = ref.Cast<duckdb::TableFunctionRef>().subquery;
            if (subquery) query(*subquery->node);
        } else if (ref.type == duckdb::TableReferenceType::SHOW_REF) {
            auto &show = ref.Cast<duckdb::ShowRef>(); name_guard(show.qualified_name);
            if (show.query) query(*show.query);
        } else if (ref.type == duckdb::TableReferenceType::PIVOT) {
            for (auto &pivot : ref.Cast<duckdb::PivotRef>().pivots) {
                if (pivot.subquery) query(*pivot.subquery);
                for (auto &expr : pivot.pivot_expressions) if (expr) expression(*expr);
                for (auto &entry : pivot.entries) if (entry.expr) expression(*entry.expr);
            }
        } else if (ref.type == duckdb::TableReferenceType::MATCH_RECOGNIZE) {
            auto &input = ref.Cast<duckdb::MatchRecognizeRef>().input;
            if (input) duckdb::ParsedExpressionIterator::EnumerateTableRefChildren(*input,
                [&](duckdb::unique_ptr<duckdb::ParsedExpression> &expr) { if (expr) expression(*expr); }, reference);
        }
    };
    query = [&](duckdb::QueryNode &node) {
        duckdb::ParsedExpressionIterator::EnumerateQueryNodeChildren(node,
            [&](duckdb::unique_ptr<duckdb::ParsedExpression> &expr) { if (expr) expression(*expr); }, reference);
    };
    using S = duckdb::StatementType;
    switch (statement.type) {
    case S::SELECT_STATEMENT: query(*statement.Cast<duckdb::SelectStatement>().node); break;
    case S::INSERT_STATEMENT: query(*statement.Cast<duckdb::InsertStatement>().node); break;
    case S::UPDATE_STATEMENT: query(*statement.Cast<duckdb::UpdateStatement>().node); break;
    case S::DELETE_STATEMENT: query(*statement.Cast<duckdb::DeleteStatement>().node); break;
    case S::MERGE_INTO_STATEMENT: query(*statement.Cast<duckdb::MergeIntoStatement>().node); break;
    case S::CREATE_STATEMENT: {
        auto &info = *statement.Cast<duckdb::CreateStatement>().info;
        if (info.type == duckdb::CatalogType::TABLE_ENTRY) {
            auto &select = info.Cast<duckdb::CreateTableInfo>().query; if (select) query(*select->node);
        } else if (info.type == duckdb::CatalogType::VIEW_ENTRY) {
            auto &select = info.Cast<duckdb::CreateViewInfo>().query; if (select) query(*select->node);
        } else if (info.type == duckdb::CatalogType::MACRO_ENTRY || info.type == duckdb::CatalogType::TABLE_MACRO_ENTRY) {
            for (auto &macro : info.Cast<duckdb::CreateMacroInfo>().macros) {
                for (auto &parameter : macro->default_parameters) if (parameter.second) expression(*parameter.second);
                if (macro->type == duckdb::MacroType::SCALAR_MACRO) {
                    auto &expr = macro->Cast<duckdb::ScalarMacroFunction>().expression;
                    if (expr) expression(*expr);
                } else {
                    auto &node = macro->Cast<duckdb::TableMacroFunction>().query_node;
                    if (node) query(*node);
                }
            }
        }
        break;
    }
    default: break;
    }
}

void GuardTokens(const std::string &sql) {
    const auto denied = [] { throw duckdb::PermissionException("This SQL operation is not allowed on the protected native connection"); };
    const auto tokens = duckdb::Parser::Tokenize(sql);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].start >= sql.size() || tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT ||
            tokens[i].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT) continue;
        const auto end = i + 1 < tokens.size() ? tokens[i + 1].start : sql.size();
        auto token = sql.substr(tokens[i].start, end - tokens[i].start);
        const auto last = token.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) continue;
        token.resize(last + 1); token = Upper(token);
        if (token.size() > 1 && token.front() == '"' && token.back() == '"') token = token.substr(1, token.size() - 2);
        std::size_t next = i + 1;
        while (next < tokens.size() && tokens[next].type == duckdb::SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) ++next;
        const bool call = next < tokens.size() && tokens[next].start < sql.size() && sql[tokens[next].start] == '(';
        if (token == "QUERY" || token == "QUERY_TABLE" || token.compare(0, 6, "QUACK_") == 0 || token == "RPC_AUTHORIZATION" ||
            (call && (token == "SET_CONFIG" || token == "SET_VARIABLE" || token.compare(0, 5, "READ_") == 0 ||
                      token.compare(0, 8, "PARQUET_") == 0 || token == "SNIFF_CSV" || token == "CSV_SCAN" ||
                      token == "JSON_SCAN" || token == "GLOB" || token == "LIST_FILES"))) denied();
    }
}

void AuthorizeStatement(duckdb::SQLStatement &statement, bool read_only) {
    using S = duckdb::StatementType;
    const auto denied = [] { throw duckdb::PermissionException("This SQL operation is not allowed on the protected native connection"); };
    if (statement.type == S::EXPLAIN_STATEMENT) {
        AuthorizeStatement(*statement.Cast<duckdb::ExplainStatement>().stmt, read_only);
    } else if (statement.type == S::PREPARE_STATEMENT) {
        AuthorizeStatement(*statement.Cast<duckdb::PrepareStatement>().statement, read_only);
    } else if (statement.type == S::SELECT_STATEMENT || statement.type == S::TRANSACTION_STATEMENT ||
               statement.type == S::EXECUTE_STATEMENT) {
        // Real bound plans are checked again before execution, including EXECUTE.
    } else {
        if (read_only) throw duckdb::PermissionException("This native account has read-only access");
        switch (statement.type) {
        case S::INSERT_STATEMENT: case S::UPDATE_STATEMENT: case S::DELETE_STATEMENT:
        case S::MERGE_INTO_STATEMENT: case S::ALTER_STATEMENT: break;
        case S::CREATE_STATEMENT: case S::DROP_STATEMENT: {
            const auto type = statement.type == S::CREATE_STATEMENT ? statement.Cast<duckdb::CreateStatement>().info->type
                                                                   : statement.Cast<duckdb::DropStatement>().info->type;
            using C = duckdb::CatalogType;
            if (type != C::TABLE_ENTRY && type != C::VIEW_ENTRY && type != C::INDEX_ENTRY && type != C::SCHEMA_ENTRY &&
                type != C::SEQUENCE_ENTRY && type != C::TYPE_ENTRY) denied();
            break;
        }
        default: denied();
        }
    }
    GuardFileReferences(statement);
    GuardTokens(statement.ToString());
}

void ValidateStoredDefinition(const std::string &sql) {
    duckdb::Parser parser;
    parser.ParseQuery(sql);
    if (parser.statements.empty()) throw duckdb::PermissionException("Missing stored SQL definition");
    for (auto &statement : parser.statements) {
        GuardTokens(statement->ToString());
        GuardFileReferences(*statement);
    }
}

void ValidateStoredDefinitions(duckdb::DuckDB &database) {
    try {
        duckdb::Connection owner(database);
        // Select definition text only. Catalog metadata never binds or executes stored expressions or bodies.
        // Qualifying the system functions also avoids a persisted user macro shadowing their names.
        auto tables = owner.Query("SELECT sql FROM system.main.duckdb_tables() WHERE NOT internal AND NOT temporary");
        if (!tables || tables->HasError()) throw duckdb::PermissionException("Stored table metadata unavailable");
        while (auto chunk = tables->Fetch()) {
            for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
                auto definition = chunk->GetValue(0, row);
                if (definition.IsNull()) throw duckdb::PermissionException("Stored table definition unavailable");
                // Complete DDL includes defaults, CHECK constraints and generated-column expressions.
                ValidateStoredDefinition(definition.GetValue<std::string>());
            }
        }
        auto views = owner.Query("SELECT sql FROM system.main.duckdb_views() WHERE NOT internal AND NOT temporary");
        if (!views || views->HasError()) throw duckdb::PermissionException("Stored view metadata unavailable");
        while (auto chunk = views->Fetch()) {
            for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
                auto definition = chunk->GetValue(0, row);
                if (definition.IsNull()) throw duckdb::PermissionException("Stored view definition unavailable");
                ValidateStoredDefinition(definition.GetValue<std::string>());
            }
        }
        auto macros = owner.Query("SELECT function_type, macro_definition FROM system.main.duckdb_functions() "
                                  "WHERE NOT internal AND function_type IN ('macro','table_macro')");
        if (!macros || macros->HasError()) throw duckdb::PermissionException("Stored macro metadata unavailable");
        while (auto chunk = macros->Fetch()) {
            for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
                auto definition = chunk->GetValue(1, row);
                if (definition.IsNull()) throw duckdb::PermissionException("Stored macro definition unavailable");
                auto sql = definition.GetValue<std::string>();
                if (chunk->GetValue(0, row).GetValue<std::string>() == "macro") sql = "SELECT (" + sql + ")";
                ValidateStoredDefinition(sql);
            }
        }
        // macro_definition omits default arguments, and index expressions are stored separately from table DDL.
        // A virtual ToSQL call reads complete stored ASTs without binding or invoking function callbacks.
        auto &context = *owner.context;
        context.RunFunctionInTransaction([&] {
            for (auto &schema : duckdb::Catalog::GetAllSchemas(context)) {
                for (auto type : {duckdb::CatalogType::MACRO_ENTRY, duckdb::CatalogType::TABLE_MACRO_ENTRY,
                                  duckdb::CatalogType::INDEX_ENTRY}) {
                    schema.get().Scan(context, type, [&](duckdb::CatalogEntry &entry) {
                        if (!entry.internal && !entry.temporary) ValidateStoredDefinition(entry.ToSQL());
                    });
                }
            }
        });
    } catch (...) {
        // Definition text and embedded function arguments may contain credentials. Do not echo them.
        throw std::runtime_error("Protected database contains an unsafe or unverifiable stored SQL definition");
    }
}

class NativeAccess final : public duckdb::QuackSecurityPolicy {
public:
    NativeAccess(const std::string &name, const AccessConfig &access, std::shared_ptr<PeerGuard> guard,
                 bool force_read_only, std::shared_ptr<std::atomic<bool>> available)
        : guard(std::move(guard)), force_read_only(force_read_only), available(std::move(available)) {
        if (!this->guard) throw std::invalid_argument("Native access requires a peer guard");
        for (const auto &rule : access.users) if (Upper(rule.database) == Upper(name)) {
            ValidateScramVerifier(rule.verifier);
            users.push_back(rule);
        }
        if (users.empty()) throw std::invalid_argument("Native database has no account grants");
        std::array<unsigned char, 32> bytes {};
        if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Native authentication initialization failed");
        std::string random_password;
        for (auto byte : bytes) { random_password += "0123456789abcdef"[byte >> 4]; random_password += "0123456789abcdef"[byte & 15]; }
        fake_verifier = MakeScramVerifier(random_password);
        OPENSSL_cleanse(random_password.data(), random_password.size());
        OPENSSL_cleanse(bytes.data(), bytes.size());
    }
    bool AdmitConnection(const std::string &peer) override { return IsAvailable() && guard->AdmitConnection(peer); }
    bool AdmitRequest(const std::string &peer) override { return IsAvailable() && guard->AdmitRequest(peer); }
    bool Authenticate(const std::string &peer, const std::string &auth, duckdb::QuackIdentity &identity) override {
        if (!IsAvailable() || guard->IsBanned(peer)) return false;
        const auto colon = auth.find(':');
        const bool format = colon != std::string::npos && colon > 0 && colon <= 63 && auth.size() <= 1088;
        const auto name = format ? auth.substr(0, colon) : std::string();
        const AccessRule *selected = nullptr;
        for (const auto &rule : users) if (rule.name == name) { selected = &rule; break; }
        auto password = format ? auth.substr(colon + 1) : std::string(32, '?');
        bool matches = false;
        try { matches = VerifyScramPassword(selected ? selected->verifier : fake_verifier, password); }
        catch (...) { OPENSSL_cleanse(password.data(), password.size()); throw; }
        OPENSSL_cleanse(password.data(), password.size());
        if (!format || !selected || !matches || guard->IsBanned(peer)) {
            guard->AuthenticationFailed(peer);
            return false;
        }
        identity.account = selected->name;
        identity.read_only = force_read_only || selected->read_only;
        return true;
    }
    void Authorize(duckdb::Connection &, const duckdb::QuackIdentity &identity, const std::string &sql) override {
        RequireAvailable();
        if (sql.empty()) return; // Transaction-state control probe, without executable SQL.
        duckdb::Parser parser;
        parser.ParseQuery(sql);
        for (auto &statement : parser.statements) AuthorizeStatement(*statement, identity.read_only);
    }
    void AuthorizeBound(const duckdb::QuackIdentity &identity, const duckdb::StatementProperties &properties) override {
        RequireAvailable();
        if (identity.read_only && !properties.modified_databases.empty())
            throw duckdb::PermissionException("This native account has read-only access");
    }
private:
    bool IsAvailable() const { return !available || available->load(); }
    void RequireAvailable() const {
        if (!IsAvailable()) throw duckdb::PermissionException("This published database is unavailable");
    }
    std::shared_ptr<PeerGuard> guard;
    std::vector<AccessRule> users;
    std::string fake_verifier;
    bool force_read_only;
    std::shared_ptr<std::atomic<bool>> available;
};

class LocalReadCopyAccess final : public duckdb::QuackSecurityPolicy {
public:
    LocalReadCopyAccess(std::string token, std::shared_ptr<std::atomic<bool>> available)
        : token(std::move(token)), available(std::move(available)) {}
    bool AdmitConnection(const std::string &) override { return available->load(); }
    bool AdmitRequest(const std::string &) override { return available->load(); }
    bool Authenticate(const std::string &, const std::string &auth, duckdb::QuackIdentity &identity) override {
        if (!available->load() || auth.size() != token.size() || CRYPTO_memcmp(auth.data(), token.data(), token.size()))
            return false;
        identity.account = "local"; identity.read_only = true; return true;
    }
    void Authorize(duckdb::Connection &, const duckdb::QuackIdentity &, const std::string &sql) override {
        RequireAvailable();
        if (sql.empty()) return;
        duckdb::Parser parser; parser.ParseQuery(sql);
        for (auto &statement : parser.statements) AuthorizeStatement(*statement, true);
    }
    void AuthorizeBound(const duckdb::QuackIdentity &, const duckdb::StatementProperties &properties) override {
        RequireAvailable();
        if (!properties.modified_databases.empty()) throw duckdb::PermissionException("This published database has read-only access");
    }
private:
    void RequireAvailable() const {
        if (!available->load()) throw duckdb::PermissionException("This published database is unavailable");
    }
    std::string token;
    std::shared_ptr<std::atomic<bool>> available;
};
} // namespace

void ConfigureNativeAccess(duckdb::DuckDB &database, const std::string &name,
                           const AccessConfig &access, std::shared_ptr<PeerGuard> guard,
                           bool force_read_only, std::shared_ptr<std::atomic<bool>> available) {
    // Audit every custom definition once before listener publication. All referenced definition chains
    // are covered, across schemas, and protected clients cannot install unaudited macros afterwards.
    ValidateStoredDefinitions(database);
    duckdb::SetQuackSecurityPolicy(*database.instance,
        duckdb::make_shared_ptr<NativeAccess>(name, access, std::move(guard), force_read_only, std::move(available)));
}
void AuditNativeStoredDefinitions(duckdb::DuckDB &database) { ValidateStoredDefinitions(database); }
void ConfigureReadCopyNativeAccess(duckdb::DuckDB &database, const std::string &token,
                                   std::shared_ptr<std::atomic<bool>> available) {
    duckdb::SetQuackSecurityPolicy(*database.instance,
        duckdb::make_shared_ptr<LocalReadCopyAccess>(token, std::move(available)));
}
} // namespace duckonwire
