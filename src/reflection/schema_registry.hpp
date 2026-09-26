#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace izanagi::schema {

enum class Status : std::uint8_t {
    found, not_found, scope_unavailable, ambiguous, registry_unavailable, invalid_query
};

struct BaseInfo {
    std::string scope;
    std::string name;
    std::uint32_t offset{};
};

struct FieldInfo {
    std::string name;
    std::string type_name;
    std::uint32_t offset{}; // relativo à classe declaradora
};

struct ClassInfo {
    std::string scope;
    std::string name;
    std::uint32_t size{};
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
};

struct ScopeInfo {
    std::string name;
    std::size_t class_count{};
};

template <typename T>
struct Result {
    Status status{Status::registry_unavailable};
    T value{};
    explicit operator bool() const noexcept { return status == Status::found; }
};

struct FieldMatch {
    std::string requested_scope;
    std::string requested_class;
    std::string declaring_scope;
    std::string declaring_class;
    FieldInfo field;
    std::uint32_t effective_offset{}; // deslocamento da base somado ao offset declarado
};

class Registry final {
public:
    static std::shared_ptr<const Registry> Build(std::vector<ClassInfo> classes,
                                                  std::uint64_t generation);

    Result<ScopeInfo> FindScope(std::string_view scope) const;
    Result<ClassInfo> FindClass(std::string_view scope, std::string_view name) const;
    Result<FieldMatch> FindDeclaredField(std::string_view scope, std::string_view name,
                                          std::string_view field) const;
    Result<FieldMatch> FindField(std::string_view scope, std::string_view name,
                                 std::string_view field) const;
    Result<bool> IsDerivedFrom(std::string_view scope, std::string_view derived,
                                std::string_view base) const;
    std::optional<FieldMatch> InheritanceProbe() const;

    std::uint64_t generation() const noexcept { return generation_; }
    std::size_t scope_count() const noexcept { return scopes_.size(); }
    std::size_t class_count() const noexcept { return classes_.size(); }
    std::size_t field_count() const noexcept { return field_count_; }

private:
    explicit Registry(std::vector<ClassInfo> classes, std::uint64_t generation);
    static std::string ScopeKey(std::string_view scope);
    static std::string ClassKey(std::string_view scope, std::string_view name);
    bool IndexAndValidate();
    Result<FieldMatch> Declared(std::size_t index, std::string_view field,
                                std::string_view requested_scope,
                                std::string_view requested_class,
                                std::uint32_t adjustment) const;

    std::vector<ClassInfo> classes_;
    std::vector<ScopeInfo> scopes_;
    std::vector<std::unordered_map<std::string, std::size_t>> fields_;
    std::unordered_map<std::string, std::size_t> scope_index_;
    std::unordered_map<std::string, std::size_t> class_index_;
    std::uint64_t generation_{};
    std::size_t field_count_{};
};

}
