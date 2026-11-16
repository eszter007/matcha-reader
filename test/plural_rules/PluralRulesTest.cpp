#include <gtest/gtest.h>

#include <vector>

#include "PluralRules.h"

using plural::form;
using plural::Rule;

namespace {
// The form index for each count in `counts`.
std::vector<int> forms(const Rule rule, const std::vector<int>& counts) {
  std::vector<int> out;
  out.reserve(counts.size());
  for (const int n : counts) out.push_back(form(rule, n));
  return out;
}
}  // namespace

TEST(PluralRules, NoPlurals) { EXPECT_EQ(forms(Rule::None, {0, 1, 2, 21}), (std::vector<int>{0, 0, 0, 0})); }

TEST(PluralRules, OneOther) { EXPECT_EQ(forms(Rule::OneOther, {0, 1, 2, 21}), (std::vector<int>{1, 0, 1, 1})); }

TEST(PluralRules, ZeroOneOther) {
  EXPECT_EQ(forms(Rule::ZeroOneOther, {0, 1, 2, 21}), (std::vector<int>{0, 0, 1, 1}));
}

// Russian: 1 день, 2 дня, 5 дней, 11 дней, 21 день, 22 дня, 112 дней.
TEST(PluralRules, EastSlavic) {
  EXPECT_EQ(forms(Rule::EastSlavic, {0, 1, 2, 4, 5, 11, 12, 14, 21, 22, 25, 101, 111, 112}),
            (std::vector<int>{2, 0, 1, 1, 2, 2, 2, 2, 0, 1, 2, 0, 2, 2}));
}

// Polish: 1 dzień, 2 dni, 5 dni, 21 dni (not one), 22 dni (few).
TEST(PluralRules, Polish) {
  EXPECT_EQ(forms(Rule::Polish, {0, 1, 2, 4, 5, 12, 21, 22, 25}), (std::vector<int>{2, 0, 1, 1, 2, 2, 2, 1, 2}));
}

TEST(PluralRules, CzechSlovak) {
  EXPECT_EQ(forms(Rule::CzechSlovak, {0, 1, 2, 4, 5, 22}), (std::vector<int>{2, 0, 1, 1, 2, 2}));
}

// Lithuanian: 1 diena, 2 dienos, 10 dienų, 11 dienų, 19 dienų, 21 diena, 22 dienos, 30 dienų.
TEST(PluralRules, Lithuanian) {
  EXPECT_EQ(forms(Rule::Lithuanian, {0, 1, 2, 9, 10, 11, 19, 21, 22, 30}),
            (std::vector<int>{2, 0, 1, 1, 2, 2, 2, 0, 1, 2}));
}

TEST(PluralRules, Bosnian) {
  EXPECT_EQ(forms(Rule::Bosnian, {0, 1, 2, 5, 11, 21, 22, 25}), (std::vector<int>{2, 0, 1, 2, 2, 0, 1, 2}));
}

// Slovenian: one 1, 101; two 2, 102; few 3-4, 103-104; other the rest.
TEST(PluralRules, Slovenian) {
  EXPECT_EQ(forms(Rule::Slovenian, {0, 1, 2, 3, 4, 5, 11, 101, 102, 103}),
            (std::vector<int>{3, 0, 1, 2, 2, 3, 3, 0, 1, 2}));
}

// Romanian: 1 zi; 0, 2-19 zile and 101-119 zile; 20 de zile.
TEST(PluralRules, Romanian) {
  EXPECT_EQ(forms(Rule::Romanian, {0, 1, 2, 19, 20, 100, 101, 119, 120}), (std::vector<int>{1, 0, 1, 1, 2, 2, 1, 1, 2}));
}

TEST(PluralRules, Arabic) {
  EXPECT_EQ(forms(Rule::Arabic, {0, 1, 2, 3, 10, 11, 99, 100, 102, 103, 111}),
            (std::vector<int>{0, 1, 2, 3, 3, 4, 4, 5, 5, 3, 4}));
}

TEST(PluralRules, Hebrew) { EXPECT_EQ(forms(Rule::Hebrew, {0, 1, 2, 3, 20}), (std::vector<int>{2, 0, 1, 2, 2})); }

TEST(PluralRules, NegativeUsesAbsolute) { EXPECT_EQ(form(Rule::EastSlavic, -21), form(Rule::EastSlavic, 21)); }
