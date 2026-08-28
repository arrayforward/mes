// 传导系数表与公共类型单测。

#include <type_traits>

#include "knowledge/conduction.h"
#include "knowledge/types.h"
#include "test_framework.h"

using namespace knowledge;

TEST(conduction_coeff_values) {
    CHECK_EQ(conduction_coeff(RelType::IS_A), 0.95);
    CHECK_EQ(conduction_coeff(RelType::PART_OF), 0.8);
    CHECK_EQ(conduction_coeff(RelType::CAUSES), 0.9);
    CHECK_EQ(conduction_coeff(RelType::TRIGGERS), 0.85);
    CHECK_EQ(conduction_coeff(RelType::REQUIRES), 0.8);
    CHECK_EQ(conduction_coeff(RelType::PREVENTS), 0.4);
    CHECK_EQ(conduction_coeff(RelType::SIMILAR), 0.7);
    CHECK_EQ(conduction_coeff(RelType::OPPOSITE), 0.5);
    CHECK_EQ(conduction_coeff(RelType::COOCCUR), 0.6);
    CHECK_EQ(conduction_coeff(RelType::BEFORE), 0.7);
    CHECK_EQ(conduction_coeff(RelType::INSTANCE_OF), 0.9);
    CHECK_EQ(conduction_coeff(RelType::OBSERVED_IN), 0.5);
    // 对立/抑制必须是最低档（防反义被当强联想）
    CHECK(conduction_coeff(RelType::OPPOSITE) <= conduction_coeff(RelType::COOCCUR));
    CHECK(conduction_coeff(RelType::PREVENTS) <= conduction_coeff(RelType::OPPOSITE));
}

TEST(rel_name_roundtrip) {
    for (int i = 0; i <= (int)RelType::OBSERVED_IN; ++i) {
        RelType t = (RelType)i;
        auto back = rel_from_name(rel_name(t));
        CHECK(back.has_value());
        CHECK(back.value() == t);
    }
    CHECK(!rel_from_name("NO_SUCH_REL").has_value());
}

TEST(rel_undirected_flags) {
    CHECK(rel_undirected(RelType::SIMILAR));
    CHECK(rel_undirected(RelType::OPPOSITE));
    CHECK(rel_undirected(RelType::COOCCUR));
    CHECK(!rel_undirected(RelType::IS_A));
    CHECK(!rel_undirected(RelType::CAUSES));
    CHECK(!rel_undirected(RelType::BEFORE));
}

TEST(error_and_ids) {
    CHECK((std::is_base_of_v<std::runtime_error, KnowledgeError>));
    CHECK_EQ(make_concept_id("战斗动作"), std::string("cp-战斗动作"));
    CHECK_EQ(make_instance_id("勇者"), std::string("in-勇者"));
}

int main() { return tfw::run_all(); }
