#include "reflection/schema_registry.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using izanagi::schema::BaseInfo;
using izanagi::schema::ClassInfo;
using izanagi::schema::FieldInfo;
using izanagi::schema::Registry;
using izanagi::schema::Status;

static bool check(bool condition, const char* what)
{
    if (!condition) std::fprintf(stderr, "failed: %s\n", what);
    return condition;
}

int main()
{
    std::vector<ClassInfo> input{
        {"client.dll", "Root", 64, {}, {{"value", "int32", 8}}},
        {"client.dll", "Other", 64, {}, {{"value", "float", 12}}},
        {"client.dll", "Derived", 128,
         {{"client.dll", "Root", 0}}, {{"own", "bool", 72}}},
        {"client.dll", "Multi", 192,
         {{"client.dll", "Root", 0}, {"client.dll", "Other", 64}}, {}},
        {"client.dll", "ExternalBase", 128,
         {{"missing.dll", "Missing", 0}}, {}},
        {"other.dll", "Missing", 64, {}, {{"foreign", "int32", 4}}},
        {"client.dll", "Derived2", 256,
         {{"client.dll", "Derived", 16}}, {}},
    };
    auto active = Registry::Build(input, 1);
    if (!check(active != nullptr, "build valid registry")) return 1;
    bool ok = true;
    ok &= check(active->FindScope("CLIENT.DLL").status == Status::found,
                "scope case insensitive");
    ok &= check(active->FindClass("client.dll", "Derived").status == Status::found,
                "class found");
    ok &= check(active->FindClass("client.dll", "derived").status == Status::not_found,
                "class case sensitive");
    ok &= check(active->FindClass("client.dll", "Absent").status == Status::not_found,
                "unknown class");
    ok &= check(active->FindDeclaredField("client.dll", "Derived", "value").status ==
                    Status::not_found, "declared field excludes base");
    const auto inherited = active->FindField("client.dll", "Derived", "value");
    ok &= check(inherited.status == Status::found &&
                    inherited.value.declaring_class == "Root" &&
                    inherited.value.effective_offset == 8,
                "inherited field and declaring class");
    ok &= check(active->FindField("client.dll", "Derived", "absent").status ==
                    Status::not_found, "unknown field");
    ok &= check(active->IsDerivedFrom("client.dll", "Derived", "Root").value,
                "inheritance relation");
    ok &= check(active->IsDerivedFrom("client.dll", "Derived2", "Root").value,
                "multi-level inheritance relation");
    ok &= check(active->FindField("client.dll", "Multi", "value").status ==
                    Status::ambiguous, "multiple-base ambiguity");
    ok &= check(active->InheritanceProbe().has_value(), "inherited field diagnostic probe");
    ok &= check(active->FindField("client.dll", "ExternalBase", "anything").status ==
                    Status::scope_unavailable, "missing dependency explicit");
    ok &= check(active->FindField("client.dll", "ExternalBase", "foreign").status ==
                    Status::scope_unavailable, "base identity includes scope");
    const auto grandchild = active->FindField("client.dll", "Derived2", "value");
    ok &= check(grandchild.status == Status::found &&
                    grandchild.value.declaring_class == "Root" &&
                    grandchild.value.effective_offset == 24,
                "multi-level inherited offset");
    auto invalid = input;
    invalid[0].bases.push_back({"client.dll", "Derived", 0});
    auto next = Registry::Build(std::move(invalid), 2);
    ok &= check(!next && active->generation() == 1 &&
                    active->FindClass("client.dll", "Root").status == Status::found,
                "failed rebuild preserves published snapshot");
    return ok ? 0 : 1;
}
