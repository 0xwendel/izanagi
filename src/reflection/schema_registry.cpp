#include "reflection/schema_registry.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace izanagi::schema {
namespace {

bool valid(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 255) return false;
    for (unsigned char c : s) if (c == 0 || c < 0x20 || c == 0x7f) return false;
    return true;
}

}

Registry::Registry(std::vector<ClassInfo> classes, std::uint64_t generation)
    : classes_(std::move(classes)), generation_(generation) {}

std::string Registry::ScopeKey(std::string_view scope)
{
    std::string key(scope);
    for (char& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return key;
}

std::string Registry::ClassKey(std::string_view scope, std::string_view name)
{
    auto key = ScopeKey(scope);
    key.push_back('\0');
    key.append(name);
    return key;
}

std::shared_ptr<const Registry> Registry::Build(std::vector<ClassInfo> classes,
                                                 std::uint64_t generation)
{
    auto next = std::shared_ptr<Registry>(new Registry(std::move(classes), generation));
    if (!next->IndexAndValidate()) return {};
    return next;
}

bool Registry::IndexAndValidate()
{
    if (classes_.size() > 100000) return false;
    fields_.resize(classes_.size());
    for (std::size_t i = 0; i < classes_.size(); ++i) {
        auto& cls = classes_[i];
        if (!valid(cls.scope) || !valid(cls.name) || cls.size > (1U << 26) ||
            cls.fields.size() > 8192 || cls.bases.size() > 32 ||
            !class_index_.emplace(ClassKey(cls.scope, cls.name), i).second) return false;
        const auto scope = ScopeKey(cls.scope);
        auto [it, inserted] = scope_index_.emplace(scope, scopes_.size());
        if (inserted) scopes_.push_back({cls.scope, 0});
        ++scopes_[it->second].class_count;
        for (std::size_t j = 0; j < cls.fields.size(); ++j) {
            const auto& f = cls.fields[j];
            if (!valid(f.name) || !valid(f.type_name) || f.offset >= cls.size ||
                !fields_[i].emplace(f.name, j).second) return false;
            ++field_count_;
        }
    }
    for (const auto& cls : classes_) {
        for (const auto& base : cls.bases) {
            const auto it = class_index_.find(ClassKey(base.scope, base.name));
            if (base.offset >= cls.size) return false;
            if (it != class_index_.end() &&
                classes_[it->second].size > cls.size - base.offset) return false;
        }
    }
    // rejeita ciclos antes da publicação para limitar o percurso das consultas.
    std::vector<std::uint8_t> color(classes_.size());
    for (std::size_t root = 0; root < classes_.size(); ++root) {
        if (color[root]) continue;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
        color[root] = 1;
        while (!stack.empty()) {
            auto& [index, edge] = stack.back();
            if (edge == classes_[index].bases.size()) {
                color[index] = 2;
                stack.pop_back();
                continue;
            }
            const auto& base = classes_[index].bases[edge++];
            const auto found = class_index_.find(ClassKey(base.scope, base.name));
            if (found == class_index_.end()) continue;
            const auto next = found->second;
            if (color[next] == 1) return false;
            if (color[next] == 0) {
                color[next] = 1;
                stack.emplace_back(next, 0);
            }
        }
    }
    return true;
}

Result<ScopeInfo> Registry::FindScope(std::string_view scope) const
{
    if (!valid(scope)) return {Status::invalid_query, {}};
    const auto it = scope_index_.find(ScopeKey(scope));
    return it == scope_index_.end() ? Result<ScopeInfo>{Status::scope_unavailable, {}}
                                    : Result<ScopeInfo>{Status::found, scopes_[it->second]};
}

Result<ClassInfo> Registry::FindClass(std::string_view scope, std::string_view name) const
{
    if (!valid(scope) || !valid(name)) return {Status::invalid_query, {}};
    if (!scope_index_.contains(ScopeKey(scope))) return {Status::scope_unavailable, {}};
    const auto it = class_index_.find(ClassKey(scope, name));
    return it == class_index_.end() ? Result<ClassInfo>{Status::not_found, {}}
                                    : Result<ClassInfo>{Status::found, classes_[it->second]};
}

Result<FieldMatch> Registry::Declared(std::size_t index, std::string_view field,
                                      std::string_view requested_scope,
                                      std::string_view requested_class,
                                      std::uint32_t adjustment) const
{
    const auto it = fields_[index].find(std::string(field));
    if (it == fields_[index].end()) return {Status::not_found, {}};
    const auto& cls = classes_[index];
    const auto& f = cls.fields[it->second];
    if (f.offset > (std::numeric_limits<std::uint32_t>::max)() - adjustment)
        return {Status::invalid_query, {}};
    return {Status::found, {std::string(requested_scope), std::string(requested_class),
                            cls.scope, cls.name, f, adjustment + f.offset}};
}

Result<FieldMatch> Registry::FindDeclaredField(std::string_view scope,
                                                std::string_view name,
                                                std::string_view field) const
{
    if (!valid(scope) || !valid(name) || !valid(field)) return {Status::invalid_query, {}};
    if (!scope_index_.contains(ScopeKey(scope))) return {Status::scope_unavailable, {}};
    const auto it = class_index_.find(ClassKey(scope, name));
    if (it == class_index_.end()) return {Status::not_found, {}};
    return Declared(it->second, field, scope, name, 0);
}

Result<FieldMatch> Registry::FindField(std::string_view scope, std::string_view name,
                                       std::string_view field) const
{
    const auto own = FindDeclaredField(scope, name, field);
    if (own.status != Status::not_found) return own;
    const auto it = class_index_.find(ClassKey(scope, name));
    if (it == class_index_.end()) return {Status::not_found, {}};
    struct Node { std::size_t index; std::uint32_t adjustment; };
    std::vector<Node> work{{it->second, 0}};
    std::unordered_set<std::string> seen;
    std::vector<FieldMatch> matches;
    bool incomplete = false;
    for (std::size_t pos = 0; pos < work.size(); ++pos) {
        const auto [index, adjustment] = work[pos];
        for (const auto& base : classes_[index].bases) {
            if (base.offset > (std::numeric_limits<std::uint32_t>::max)() - adjustment)
                return {Status::invalid_query, {}};
            const auto next_offset = adjustment + base.offset;
            const auto found_class = class_index_.find(ClassKey(base.scope, base.name));
            if (found_class == class_index_.end()) {
                incomplete = true;
                continue;
            }
            const auto next = found_class->second;
            auto path = ClassKey(base.scope, base.name);
            path.append(reinterpret_cast<const char*>(&next_offset), sizeof(next_offset));
            if (!seen.insert(path).second) continue;
            const auto found = Declared(next, field, scope, name, next_offset);
            if (found) matches.push_back(found.value);
            if (!found) work.push_back({next, next_offset});
            if (work.size() > 100000) return {Status::invalid_query, {}};
        }
    }
    if (incomplete) return {Status::scope_unavailable, {}};
    if (matches.empty()) return {Status::not_found, {}};
    const auto& first = matches.front();
    for (const auto& match : matches) {
        if (match.declaring_scope != first.declaring_scope ||
            match.declaring_class != first.declaring_class ||
            match.effective_offset != first.effective_offset) return {Status::ambiguous, {}};
    }
    return {Status::found, first};
}

Result<bool> Registry::IsDerivedFrom(std::string_view scope, std::string_view derived,
                                     std::string_view base) const
{
    if (!valid(scope) || !valid(derived) || !valid(base)) return {Status::invalid_query, false};
    if (!scope_index_.contains(ScopeKey(scope))) return {Status::scope_unavailable, false};
    const auto start = class_index_.find(ClassKey(scope, derived));
    if (start == class_index_.end()) return {Status::not_found, false};
    std::vector<std::size_t> work{start->second};
    std::unordered_set<std::size_t> seen{start->second};
    bool incomplete = false;
    for (std::size_t pos = 0; pos < work.size(); ++pos) {
        for (const auto& edge : classes_[work[pos]].bases) {
            if (edge.name == base && ScopeKey(edge.scope) == ScopeKey(scope))
                return {Status::found, true};
            const auto found = class_index_.find(ClassKey(edge.scope, edge.name));
            if (found == class_index_.end()) {
                incomplete = true;
                continue;
            }
            const auto next = found->second;
            if (seen.insert(next).second) work.push_back(next);
        }
    }
    return incomplete ? Result<bool>{Status::scope_unavailable, false}
                      : Result<bool>{Status::found, false};
}

std::optional<FieldMatch> Registry::InheritanceProbe() const
{
    for (const auto& cls : classes_) {
        for (const auto& base : cls.bases) {
            const auto it = class_index_.find(ClassKey(base.scope, base.name));
            if (it == class_index_.end()) continue;
            for (const auto& field : classes_[it->second].fields) {
                if (fields_[class_index_.at(ClassKey(cls.scope, cls.name))].contains(field.name))
                    continue;
                const auto result = FindField(cls.scope, cls.name, field.name);
                if (result && result.value.declaring_scope == base.scope &&
                    result.value.declaring_class == base.name &&
                    result.value.field.name == field.name)
                    return result.value;
            }
        }
    }
    return std::nullopt;
}

}
